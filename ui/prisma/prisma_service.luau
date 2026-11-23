--!nonstrict
-- Prisma Control Center - Headless Telemetry & Hardware Service
-- Manages background polling of AMD GPU hardware telemetry and color parameters,
-- maintains historical ring-buffers, debounces mutations, and manages profiles and automation.

noctalia.setUpdateInterval(50)

local TICKS_ACTIVE = 20    -- 20 * 50ms = 1000ms (1s) when panel is open
local TICKS_IDLE = 100     -- 100 * 50ms = 5000ms (5s) in background
local TICKS_AUTOMATION_REFRESH = 200 -- Recheck external CLI changes every 10s
local DEBOUNCE_TICKS = 1   -- 1 * 50ms = 50ms debounce window for slider adjustments
local MAX_HISTORY = 60

local tickCount = 0
local panelOpen = false
local panelTab = nil
local isPolling = false
local isMutating = false
local mutationRevision = 0
local repollStatus = false
local queuedStatusCallback = nil
local needsDeviceSnapshot = false

local isAvailable = false
local isDeviceAvailable = false
local deviceState = {
    enabled = 1,
    saturation = 100,
    contrast = 100,
    brightness = 0,
}
local telemetryState = {
    card = "",
    pci_id = "",
    device_id = "",
    gpu_name = "",
    gpu_type = "",
    is_apu = false,
    gpu_busy_percent = 0,
    sclk_mhz = 0,
    vram_used_mib = 0,
    vram_total_mib = 0,
    gtt_used_mib = 0,
    gtt_total_mib = 0,
    temp_edge_c = 0,
    temp_junction_c = 0,
    power_watts = 0,
    power_cap_watts = 0,
}

-- Ring-buffers (up to 60 historical samples)
local historyGpu = {}
local historyTempJunc = {}
local historyPower = {}
local historySclk = {}

local savedProfiles = {}
local profilesError = nil
local automation = { enabled = false, rules = {}, niri_available = true }
local automationLoaded = false
local startupHandled = false
local restoreStartupProfile
local isWatchingWindow = false
local retryWindowProbe = false
local nextWindowTick = TICKS_ACTIVE
local lastFocusedWindow = nil
local observedWindow = nil
local observedAppId = nil
local observationRevision = 0
local snapshotRequest = nil
local evaluatedWindow = nil
local evaluatedPattern = nil
local evaluatedProfile = nil
local overriddenWindow = nil
local preRuleState = nil
local lastErrorMessage = ""
local mutationErrorSeq = 0

-- Debounce tracking
local isDirty = false
local dirtyTicks = 0
local pendingState = {
    enabled = 1,
    saturation = 100,
    contrast = 100,
    brightness = 0,
}

local function tr(key, fallback)
    if type(noctalia.tr) == "function" then
        local ok, text = pcall(noctalia.tr, key)
        if ok and type(text) == "string" and text ~= "" and text ~= key then return text end
    end
    return fallback
end

local function log(msg)
    noctalia.log(`prisma_service: {msg}`)
end

local function clamp(val, lo, hi)
    return math.max(lo, math.min(hi, val))
end

local function shellQuote(s)
    return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

local function runCommand(command, callback)
    if noctalia.runAsync(command, callback) == false then
        callback({ exitCode = 1, stdout = "", stderr = "" })
    end
end

local function appendHistory(buf, val)
    table.insert(buf, val)
    if #buf > MAX_HISTORY then
        table.remove(buf, 1)
    end
end

local function publishStatus()
    local currentDev = if isDirty or isMutating then pendingState else deviceState
    noctalia.state.set("status", {
        available = isAvailable,
        device_available = isDeviceAvailable,
        device = {
            available = isDeviceAvailable,
            enabled = if isDeviceAvailable then currentDev.enabled else 0,
            saturation = currentDev.saturation,
            contrast = currentDev.contrast,
            brightness = currentDev.brightness,
        },
        telemetry = {
            card = telemetryState.card,
            pci_id = telemetryState.pci_id,
            device_id = telemetryState.device_id,
            gpu_name = telemetryState.gpu_name,
            gpu_type = telemetryState.gpu_type,
            is_apu = telemetryState.is_apu,
            gpu_busy_percent = telemetryState.gpu_busy_percent,
            sclk_mhz = telemetryState.sclk_mhz,
            vram_used_mib = telemetryState.vram_used_mib,
            vram_total_mib = telemetryState.vram_total_mib,
            gtt_used_mib = telemetryState.gtt_used_mib,
            gtt_total_mib = telemetryState.gtt_total_mib,
            temp_edge_c = telemetryState.temp_edge_c,
            temp_junction_c = telemetryState.temp_junction_c,
            power_watts = telemetryState.power_watts,
            power_cap_watts = telemetryState.power_cap_watts,
        },
        history = {
            gpu_busy = historyGpu,
            temp_junction = historyTempJunc,
            power = historyPower,
            sclk = historySclk,
        },
        profiles = savedProfiles,
        profiles_error = profilesError,
        automation = {
            enabled = automation.enabled,
            default_profile = automation.default_profile,
            rules = automation.rules,
            error = automation.error,
            niri_available = automation.niri_available,
            active_rule = automation.active_rule,
            active_app_id = automation.active_app_id,
            last_app_id = automation.last_app_id,
        },
        panel_open = panelOpen,
        error = if lastErrorMessage ~= "" then lastErrorMessage else nil,
        mutation_error_seq = mutationErrorSeq,
    })
end

local function pollStatus(force, onComplete)
    if isPolling then
        repollStatus = repollStatus or force == true or onComplete ~= nil
        if onComplete then queuedStatusCallback = onComplete end
        return
    end
    isPolling = true
    local revision = mutationRevision

    runCommand("prisma-ctl status --json", function(result)
        isPolling = false
        local freshDeviceSnapshot = false
        if result.exitCode == 0 and type(result.stdout) == "string" and result.stdout ~= "" then
            local ok, parsed = pcall(function()
                return noctalia.json.decode(result.stdout)
            end)

            if ok and type(parsed) == "table" then
                isAvailable = true

                -- Safely parse device state with fallback defaults
                local dev = parsed.device
                if type(dev) == "table" then
                    if dev.available ~= nil then
                        isDeviceAvailable = (dev.available == true)
                    else
                        isDeviceAvailable = true
                    end
                    local hwEnabled = if type(dev.enabled) == "number" then dev.enabled else 1
                    local hwSat = if type(dev.saturation) == "number" then dev.saturation else 100
                    local hwCon = if type(dev.contrast) == "number" then dev.contrast else 100
                    local hwBri = if type(dev.brightness) == "number" then dev.brightness else 0

                    if not isDirty and not isMutating and revision == mutationRevision then
                        deviceState.enabled = hwEnabled
                        deviceState.saturation = hwSat
                        deviceState.contrast = hwCon
                        deviceState.brightness = hwBri

                        pendingState.enabled = hwEnabled
                        pendingState.saturation = hwSat
                        pendingState.contrast = hwCon
                        pendingState.brightness = hwBri
                        needsDeviceSnapshot = false
                        freshDeviceSnapshot = isDeviceAvailable
                    end
                else
                    isDeviceAvailable = false
                end

                -- Safely parse hardware telemetry handling nulls
                local telem = parsed.telemetry
                if type(telem) == "table" then
                    telemetryState.card = if type(telem.card) == "string" and telem.card ~= "" then telem.card else "amdgpu"
                    telemetryState.pci_id = if type(telem.pci_id) == "string" then telem.pci_id else ""
                    telemetryState.device_id = if type(telem.device_id) == "string" then telem.device_id else ""
                    telemetryState.gpu_name = if type(telem.gpu_name) == "string" then telem.gpu_name else ""
                    telemetryState.gpu_type = if type(telem.gpu_type) == "string" then telem.gpu_type else ""
                    telemetryState.is_apu = (telem.is_apu == true)
                    telemetryState.gpu_busy_percent = if type(telem.gpu_busy_percent) == "number" then telem.gpu_busy_percent else 0
                    telemetryState.sclk_mhz = if type(telem.sclk_mhz) == "number" then telem.sclk_mhz else 0
                    telemetryState.vram_used_mib = if type(telem.vram_used_mib) == "number" then telem.vram_used_mib else 0
                    telemetryState.vram_total_mib = if type(telem.vram_total_mib) == "number" then telem.vram_total_mib else 0
                    telemetryState.gtt_used_mib = if type(telem.gtt_used_mib) == "number" then telem.gtt_used_mib else 0
                    telemetryState.gtt_total_mib = if type(telem.gtt_total_mib) == "number" then telem.gtt_total_mib else 0
                    telemetryState.temp_edge_c = if type(telem.temp_edge_c) == "number" then telem.temp_edge_c else 0
                    telemetryState.temp_junction_c = if type(telem.temp_junction_c) == "number" then telem.temp_junction_c else 0
                    telemetryState.power_watts = if type(telem.power_watts) == "number" then telem.power_watts else 0
                    telemetryState.power_cap_watts = if type(telem.power_cap_watts) == "number" then telem.power_cap_watts else 0
                end

                local effectiveTemp = if telemetryState.temp_junction_c > 0 then telemetryState.temp_junction_c else telemetryState.temp_edge_c

                appendHistory(historyGpu, telemetryState.gpu_busy_percent)
                appendHistory(historyTempJunc, effectiveTemp)
                appendHistory(historyPower, telemetryState.power_watts)
                appendHistory(historySclk, telemetryState.sclk_mhz)

                lastErrorMessage = ""
            else
                isAvailable = false
                isDeviceAvailable = false
                lastErrorMessage = tr("errors.telemetry", "Falha ao decodificar JSON de telemetria")
            end
        else
            isAvailable = false
            isDeviceAvailable = false
            if type(result.stderr) == "string" and result.stderr ~= "" then
                lastErrorMessage = noctalia.string.trim(result.stderr)
            else
                lastErrorMessage = tr("errors.offline", "Módulo chroma_amd não carregado ou permissão negada")
            end
        end

        publishStatus()
        if repollStatus then
            local callback = queuedStatusCallback or onComplete
            queuedStatusCallback = nil
            repollStatus = false
            pollStatus(false, callback)
        else
            restoreStartupProfile()
            if onComplete then onComplete(freshDeviceSnapshot) end
        end
    end)
end

local function isIntegerInRange(value, low, high)
    return type(value) == "number" and value >= low and value <= high and value == math.floor(value)
end

local function profilesCompatibilityError()
    return tr("errors.cli_profiles_compatibility", "Atualize o prisma-ctl junto com o plugin e recarregue a lista de perfis.")
end

local profilesRequest = 0
local function refreshProfiles()
    profilesRequest += 1
    local request = profilesRequest
    runCommand("prisma-ctl profile list --json", function(result)
        if request ~= profilesRequest then return end
        if result.exitCode ~= 0 or type(result.stdout) ~= "string" then
            profilesError = tr("errors.profiles_list", "Falha ao carregar perfis salvos")
            publishStatus()
            return
        end
        -- Empty JSON objects and arrays both decode to empty Luau tables.
        local ok, profiles = pcall(noctalia.json.decode, result.stdout)
        if not ok or type(profiles) ~= "table" or not result.stdout:match("^%s*%[") then
            if result.stdout:match("^Saved profiles %(") or result.stdout:match("^No saved profiles found in ") then
                profilesError = profilesCompatibilityError()
            else
                profilesError = tr("errors.profiles_list", "Falha ao carregar perfis salvos")
            end
            publishStatus()
            return
        end

        profilesError = nil
        local list = {}
        for _, profile in ipairs(profiles) do
            if type(profile) == "table" and type(profile.name) == "string" and profile.name ~= ""
                and isIntegerInRange(profile.saturation, 0, 400)
                and isIntegerInRange(profile.contrast, 10, 300)
                and isIntegerInRange(profile.brightness, -100, 100)
                and isIntegerInRange(profile.enabled, 0, 1) then
                table.insert(list, {
                    name = profile.name,
                    saturation = profile.saturation,
                    contrast = profile.contrast,
                    brightness = profile.brightness,
                    enabled = profile.enabled,
                })
            end
        end
        savedProfiles = list
        publishStatus()
    end)
end

local function setCommand(state)
    return `prisma-ctl set -s {state.saturation} -c {state.contrast} -b {state.brightness} -e {state.enabled}`
end

local function profileCommand(name)
    return `prisma-ctl profile apply {shellQuote(name)}`
end

local function recordMutationError(result, key, fallback)
    mutationErrorSeq += 1
    local message = noctalia.string.trim(result.stderr or "")
    lastErrorMessage = if message ~= "" then message else tr(key, fallback)
end

local flushSetCommand
local queuedProfileMutation
local function runDeviceMutation(command, complete)
    isMutating = true
    mutationRevision += 1
    publishStatus()

    runCommand(command, function(result)
        isMutating = false
        complete(result)
        publishStatus()

        if queuedProfileMutation then
            local queued = queuedProfileMutation
            queuedProfileMutation = nil
            queued()
        elseif isDirty then
            flushSetCommand()
        elseif result.exitCode == 0 or needsDeviceSnapshot then
            pollStatus(true)
        end
    end)
end

flushSetCommand = function()
    if isMutating then return end
    if not isDeviceAvailable then
        isDirty = false
        mutationErrorSeq += 1
        lastErrorMessage = tr("errors.write", "Módulo chroma_amd não disponível para gravação")
        publishStatus()
        return
    end
    isDirty = false
    local target = table.clone(pendingState)
    runDeviceMutation(setCommand(target), function(result)
        if result.exitCode == 0 then
            deviceState = target
            needsDeviceSnapshot = false
            lastErrorMessage = ""
        else
            isDirty = false
            pendingState = table.clone(deviceState)
            recordMutationError(result, "errors.set", "Falha ao aplicar parâmetros")
            log(`Erro ao aplicar comando set: {lastErrorMessage}`)
        end
    end)
end

local function runProfileMutation(command, errorKey, fallback, onSuccess)
    runDeviceMutation(command, function(result)
        if result.exitCode == 0 then
            needsDeviceSnapshot = true
            lastErrorMessage = ""
            if onSuccess then onSuccess() end
        else
            recordMutationError(result, errorKey, fallback)
        end
    end)
end

restoreStartupProfile = function()
    if startupHandled or not automationLoaded or not isDeviceAvailable or isMutating or isDirty then return end
    startupHandled = true
    if automation.default_profile then
        runProfileMutation(profileCommand(automation.default_profile), "errors.apply", "Falha ao aplicar perfil")
    end
end

local function clearActiveRule()
    automation.active_rule = nil
    automation.active_app_id = nil
end

local automationRequest = 0
local function refreshAutomation()
    automationRequest += 1
    local request = automationRequest
    runCommand("prisma-ctl rule list --json", function(result)
        if request ~= automationRequest then return end
        local ok, config = false, nil
        if result.exitCode == 0 and type(result.stdout) == "string" then
            ok, config = pcall(noctalia.json.decode, result.stdout)
        end
        if not ok or type(config) ~= "table" or type(config.enabled) ~= "boolean" or type(config.rules) ~= "table" then
            if type(result.stderr) == "string" and result.stderr:find("Unknown command: 'rule'", 1, true) then
                automation.error = profilesCompatibilityError()
            else
                automation.error = tr("errors.automation_list", "Falha ao carregar regras de automação")
            end
            publishStatus()
            return
        end

        automation.error = nil
        local rules = {}
        for _, rule in ipairs(config.rules) do
            if type(rule) == "table" and type(rule.pattern) == "string" and rule.pattern ~= ""
                and type(rule.profile) == "string" and rule.profile ~= "" then
                table.insert(rules, { pattern = rule.pattern, profile = rule.profile })
            end
        end
        automationLoaded = true
        automation.enabled = config.enabled
        automation.default_profile = if type(config.default_profile) == "string" and config.default_profile ~= ""
            then config.default_profile else nil
        automation.rules = rules
        if not automation.enabled or #rules == 0 then
            clearActiveRule()
            evaluatedWindow, evaluatedPattern, evaluatedProfile = nil, nil, nil
            overriddenWindow, preRuleState = nil, nil
        end
        publishStatus()
        restoreStartupProfile()
    end)
end

local function updateAutomation(arguments)
    runCommand(`prisma-ctl rule {arguments}`, function(result)
        if result.exitCode == 0 then
            lastErrorMessage = ""
            refreshAutomation()
        else
            local message = noctalia.string.trim(result.stderr or "")
            lastErrorMessage = if message ~= "" then message else tr("errors.automation", "Falha ao atualizar regras de automação")
            publishStatus()
        end
    end)
end

local function globMatches(pattern, appId)
    local patternIndex, appIndex = 1, 1
    local lastStar, starEnd
    while appIndex <= #appId do
        if pattern:sub(patternIndex, patternIndex) == "*" then
            lastStar, starEnd = patternIndex, appIndex
            patternIndex += 1
        elseif pattern:byte(patternIndex) == appId:byte(appIndex) then
            patternIndex += 1
            appIndex += 1
        elseif lastStar then
            starEnd += 1
            patternIndex, appIndex = lastStar + 1, starEnd
        else
            return false
        end
    end
    return pattern:find("[^*]", patternIndex) == nil
end

local function matchRule(appId)
    if not appId then return nil end
    local folded = string.lower(appId)
    local best, bestPattern, bestScore = nil, nil, -1
    for _, rule in ipairs(automation.rules) do
        local pattern = string.lower(rule.pattern)
        local _, stars = pattern:gsub("%*", "")
        if stars == 0 then
            if pattern == folded then return rule end
        else
            local score = #pattern - stars
            if (score > bestScore or (score == bestScore and pattern < bestPattern)) and globMatches(pattern, folded) then
                best, bestPattern, bestScore = rule, pattern, score
            end
        end
    end
    return best
end

local function hasAutomationRules()
    return automation.enabled and #automation.rules > 0
end

local function applyWindowRule(window, appId, hasFreshSnapshot)
    if panelOpen or isMutating or isDirty or not isDeviceAvailable or needsDeviceSnapshot
        or not startupHandled or not hasAutomationRules() or overriddenWindow == window then return end

    local rule = matchRule(appId)
    local pattern = if rule then rule.pattern else nil
    local profile = if rule then rule.profile else nil
    if evaluatedWindow == window and evaluatedPattern == pattern and evaluatedProfile == profile then return end
    if rule and not preRuleState and not hasFreshSnapshot then
        local request = observationRevision
        if snapshotRequest == request then return end
        snapshotRequest = request
        pollStatus(false, function(fresh)
            if snapshotRequest ~= request then return end
            snapshotRequest = nil
            if fresh and observationRevision == request then
                applyWindowRule(window, appId, true)
            end
        end)
        return
    end
    evaluatedWindow, evaluatedPattern, evaluatedProfile = window, pattern, profile

    if rule then
        local snapshot = preRuleState or table.clone(deviceState)
        runProfileMutation(profileCommand(profile), "errors.apply", "Falha ao aplicar perfil", function()
            if not hasAutomationRules() then return end
            preRuleState = snapshot
            if overriddenWindow ~= window then
                automation.active_rule = pattern
                automation.active_app_id = appId
            end
        end)
    elseif preRuleState then
        local function finishRestoration()
            preRuleState = nil
            clearActiveRule()
        end
        if automation.default_profile then
            runProfileMutation(profileCommand(automation.default_profile), "errors.apply", "Falha ao aplicar perfil", finishRestoration)
        else
            local snapshot = preRuleState
            runDeviceMutation(setCommand(snapshot), function(result)
                if result.exitCode == 0 then
                    deviceState = snapshot
                    if not isDirty then pendingState = table.clone(snapshot) end
                    lastErrorMessage = ""
                    finishRestoration()
                else
                    recordMutationError(result, "errors.set", "Falha ao aplicar parâmetros")
                end
            end)
        end
    end
end

local function watchWindow()
    local capture = panelOpen and panelTab == "profiles"
    if isWatchingWindow or tickCount < nextWindowTick or not (hasAutomationRules() or capture)
        or not (automation.niri_available or (capture and retryWindowProbe)) then return end

    isWatchingWindow = true
    retryWindowProbe = false
    nextWindowTick = tickCount + TICKS_ACTIVE
    local started = noctalia.runAsync("niri msg -j focused-window", function(result)
        isWatchingWindow = false
        local wasAvailable, lastAppId = automation.niri_available, automation.last_app_id
        local ok, focused, noWindow
        if result.exitCode == 0 and type(result.stdout) == "string" then
            ok, focused = pcall(noctalia.json.decode, result.stdout)
            noWindow = result.stdout:match("^%s*null%s*$") ~= nil
        end

        local appId = if type(focused) == "table" and type(focused.app_id) == "string" and focused.app_id ~= ""
            then focused.app_id else nil
        local window = if type(focused) == "table" and type(focused.id) == "number" then focused.id else appId
        automation.niri_available = ok == true and (noWindow or window ~= nil)
        if window ~= observedWindow or appId ~= observedAppId then
            observedWindow, observedAppId = window, appId
            observationRevision += 1
        end
        if automation.niri_available and window ~= nil then
            lastFocusedWindow = window
            if appId then automation.last_app_id = appId end
            if overriddenWindow and overriddenWindow ~= window then
                overriddenWindow = nil
                evaluatedWindow = nil
            end
            applyWindowRule(window, appId)
        end
        if wasAvailable ~= automation.niri_available or lastAppId ~= automation.last_app_id then
            publishStatus()
        end
    end)
    if started == false then
        isWatchingWindow = false
        retryWindowProbe = capture
    end
end

local function overrideWindowRule()
    if preRuleState or (evaluatedWindow and evaluatedPattern) then
        overriddenWindow = lastFocusedWindow or evaluatedWindow
        clearActiveRule()
    end
end

local function manuallyApplyProfile(command, errorKey, fallback)
    overrideWindowRule()
    isDirty = false
    local function apply()
        runProfileMutation(command, errorKey, fallback)
    end
    if isMutating then
        queuedProfileMutation = apply
        publishStatus()
    else
        apply()
    end
end

local function handleCommand(cmd)
    if type(cmd) ~= "table" or type(cmd.action) ~= "string" then return end

    if cmd.action == "set" then
        overrideWindowRule()
        if type(cmd.saturation) == "number" then
            pendingState.saturation = clamp(math.floor(cmd.saturation + 0.5), 0, 400)
        end
        if type(cmd.contrast) == "number" then
            pendingState.contrast = clamp(math.floor(cmd.contrast + 0.5), 10, 300)
        end
        if type(cmd.brightness) == "number" then
            pendingState.brightness = clamp(math.floor(cmd.brightness + 0.5), -100, 100)
        end
        if type(cmd.enabled) == "number" then
            pendingState.enabled = if cmd.enabled ~= 0 then 1 else 0
        end

        publishStatus()

        if cmd.immediate then
            if isMutating then
                isDirty = true
                dirtyTicks = 0
            else
                flushSetCommand()
            end
        else
            isDirty = true
            dirtyTicks = 0
        end

    elseif cmd.action == "profile_save" then
        local name = cmd.name
        if type(name) == "string" and name ~= "" then
            noctalia.runAsync(`prisma-ctl profile save {shellQuote(name)}`, function(result)
                if result.exitCode == 0 then
                    lastErrorMessage = ""
                    refreshProfiles()
                else
                    lastErrorMessage = noctalia.string.trim(result.stderr or tr("errors.save", "Falha ao salvar perfil"))
                    publishStatus()
                end
            end)
        end

    elseif cmd.action == "profile_apply" then
        local name = cmd.name
        if type(name) == "string" and name ~= "" then
            manuallyApplyProfile(profileCommand(name), "errors.apply", "Falha ao aplicar perfil")
        end

    elseif cmd.action == "profile_delete" then
        local name = cmd.name
        if type(name) == "string" and name ~= "" then
            noctalia.runAsync(`prisma-ctl profile delete {shellQuote(name)}`, function(result)
                if result.exitCode == 0 then
                    lastErrorMessage = ""
                    refreshProfiles()
                    refreshAutomation()
                else
                    lastErrorMessage = noctalia.string.trim(result.stderr or tr("errors.delete", "Falha ao excluir perfil"))
                    publishStatus()
                end
            end)
        end

    elseif cmd.action == "profile_reset" then
        manuallyApplyProfile("prisma-ctl profile reset", "errors.reset", "Falha ao resetar parâmetros")

    elseif cmd.action == "profile_list" then
        refreshProfiles()
        refreshAutomation()

    elseif cmd.action == "rule_list" then
        refreshAutomation()

    elseif cmd.action == "rule_toggle" then
        local enabled = if type(cmd.enabled) == "boolean" then cmd.enabled else not automation.enabled
        updateAutomation(if enabled then "enable" else "disable")

    elseif cmd.action == "rule_set" then
        if type(cmd.pattern) == "string" and cmd.pattern ~= "" and type(cmd.profile) == "string" and cmd.profile ~= "" then
            updateAutomation(`set {shellQuote(cmd.pattern)} {shellQuote(cmd.profile)}`)
        end

    elseif cmd.action == "rule_delete" then
        if type(cmd.pattern) == "string" and cmd.pattern ~= "" then
            updateAutomation(`delete {shellQuote(cmd.pattern)}`)
        end

    elseif cmd.action == "rule_set_default" then
        if type(cmd.profile) == "string" and cmd.profile ~= "" then
            local profile = shellQuote(cmd.profile)
            updateAutomation(if cmd.profile == "--clear" then `default -- {profile}` else `default {profile}`)
        end

    elseif cmd.action == "rule_clear_default" then
        updateAutomation("default --clear")

    elseif cmd.action == "panel_state" then
        local wasOpen = panelOpen
        local wasCapturing = panelOpen and panelTab == "profiles"
        panelOpen = if cmd.open then true else false
        panelTab = if panelOpen then cmd.tab else nil
        if panelOpen and panelTab == "profiles" and not wasCapturing and not automation.niri_available then
            retryWindowProbe = true
        end
        if panelOpen and not wasOpen then
            pollStatus()
            refreshProfiles()
        end
        publishStatus()
    end
end

function update()
    tickCount += 1

    -- Debounce processor for rapid slider adjustments
    if isDirty then
        dirtyTicks += 1
        if dirtyTicks >= DEBOUNCE_TICKS then
            flushSetCommand()
        end
    end

    -- Periodic polling interval
    local interval = if panelOpen then TICKS_ACTIVE else TICKS_IDLE
    if tickCount % interval == 0 then
        pollStatus()
    end
    if tickCount % TICKS_AUTOMATION_REFRESH == 0 then
        refreshAutomation()
    end
    restoreStartupProfile()
    watchWindow()
end

-- Listen for commands from the widget or interactive panel
noctalia.state.watch("command", handleCommand)

-- Initial probe and load
pollStatus()
refreshProfiles()
refreshAutomation()
