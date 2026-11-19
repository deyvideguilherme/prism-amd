--!nonstrict
-- Prisma Control Center - Headless Telemetry, Hardware & Automation Service
-- Manages background polling of AMD GPU hardware telemetry, color parameters,
-- maintains historical ring-buffers, debounces mutations, manages profiles,
-- and handles dynamic open window discovery and automatic per-game profile switching.

noctalia.setUpdateInterval(50)

local TICKS_ACTIVE = 20    -- 20 * 50ms = 1000ms (1s) when panel is open / window polling
local TICKS_IDLE = 100     -- 100 * 50ms = 5000ms (5s) in background
local DEBOUNCE_TICKS = 1   -- 1 * 50ms = 50ms debounce window for slider adjustments
local MAX_HISTORY = 60

local PRESETS = {
    padrao = { saturation = 100, contrast = 100, brightness = 0, enabled = 1 },
    competitivo = { saturation = 160, contrast = 115, brightness = 10, enabled = 1 },
    cinema = { saturation = 120, contrast = 105, brightness = -5, enabled = 1 },
    monocromatico = { saturation = 0, contrast = 110, brightness = 0, enabled = 1 },
}

local tickCount = 0
local panelOpen = false
local isPolling = false
local isMutating = false
local isQueryingWindows = false

local isAvailable = false
local deviceState = {
    enabled = 1,
    saturation = 100,
    contrast = 100,
    brightness = 0,
}
local telemetryState = {
    card = "card1",
    pci_id = "",
    device_id = "",
    gpu_busy_percent = 0,
    sclk_mhz = 0,
    vram_used_mib = 0,
    vram_total_mib = 0,
    temp_edge_c = 0,
    temp_junction_c = 0,
    power_watts = 0,
}

-- Ring-buffers (up to 60 historical samples)
local historyGpu = {}
local historyTempJunc = {}
local historyPower = {}
local historySclk = {}

local savedProfiles = {}
local lastErrorMessage = ""

-- Debounce tracking
local isDirty = false
local dirtyTicks = 0
local pendingState = {
    enabled = 1,
    saturation = 100,
    contrast = 100,
    brightness = 0,
}

-- Game auto-switching, open windows discovery & app rules
local openApps = {}
local focusedAppId = ""
local focusedAppTitle = ""
local autoSwitchEnabled = true
local appRules = {}
local activeAutoProfile = nil
local fallbackState = nil

local configDir = (noctalia.getenv("XDG_CONFIG_HOME") or ((noctalia.getenv("HOME") or "") .. "/.config")) .. "/prisma"
local rulesFilePath = configDir .. "/app_rules.json"

local function log(msg)
    noctalia.log(`prisma_service: {msg}`)
end

local function clamp(val, lo, hi)
    return math.max(lo, math.min(hi, val))
end

local function shellQuote(s)
    return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

local function appendHistory(buf, val)
    table.insert(buf, val)
    if #buf > MAX_HISTORY then
        table.remove(buf, 1)
    end
end

local function loadAppRules()
    local contents = noctalia.readFile(rulesFilePath)
    if contents and contents ~= "" then
        local ok, parsed = pcall(function()
            return noctalia.json.decode(contents)
        end)
        if ok and type(parsed) == "table" then
            if type(parsed.auto_switch) == "boolean" then
                autoSwitchEnabled = parsed.auto_switch
            end
            if type(parsed.rules) == "table" then
                appRules = parsed.rules
            else
                local found = {}
                for k, v in pairs(parsed) do
                    if k ~= "auto_switch" and type(v) == "table" then
                        found[k] = v
                    end
                end
                appRules = found
            end
        end
    end
end

local function saveAppRules()
    noctalia.mkdirAll(configDir)
    local data = {
        auto_switch = autoSwitchEnabled,
        rules = appRules,
    }
    local ok, encoded = pcall(function()
        return noctalia.json.encode(data)
    end)
    if ok and type(encoded) == "string" then
        noctalia.writeFile(rulesFilePath, encoded)
    end
end

local function publishStatus()
    local status = {
        available = isAvailable,
        device = {
            enabled = deviceState.enabled,
            saturation = deviceState.saturation,
            contrast = deviceState.contrast,
            brightness = deviceState.brightness,
        },
        telemetry = {
            card = telemetryState.card,
            pci_id = telemetryState.pci_id,
            device_id = telemetryState.device_id,
            gpu_busy_percent = telemetryState.gpu_busy_percent,
            sclk_mhz = telemetryState.sclk_mhz,
            vram_used_mib = telemetryState.vram_used_mib,
            vram_total_mib = telemetryState.vram_total_mib,
            temp_edge_c = telemetryState.temp_edge_c,
            temp_junction_c = telemetryState.temp_junction_c,
            power_watts = telemetryState.power_watts,
        },
        history = {
            gpu_busy = historyGpu,
            temp_junction = historyTempJunc,
            power = historyPower,
            sclk = historySclk,
        },
        profiles = savedProfiles,
        panel_open = panelOpen,
        error = if lastErrorMessage ~= "" then lastErrorMessage else nil,
        openApps = openApps,
        focusedAppId = focusedAppId,
        focusedAppTitle = focusedAppTitle,
        autoSwitchEnabled = autoSwitchEnabled,
        appRules = appRules,
        activeAutoProfile = activeAutoProfile,
    }

    noctalia.state.set("status", status)
    noctalia.state.set("openApps", openApps)
    noctalia.state.set("focusedAppId", focusedAppId)
    noctalia.state.set("focusedAppTitle", focusedAppTitle)
    noctalia.state.set("autoSwitchEnabled", autoSwitchEnabled)
    noctalia.state.set("appRules", appRules)
    noctalia.state.set("activeAutoProfile", activeAutoProfile)
end

local function applyProfileByName(profileName, callback)
    local lower = string.lower(profileName)
    local preset = PRESETS[lower]
    if preset then
        local cmd = `prisma-ctl set -s {preset.saturation} -c {preset.contrast} -b {preset.brightness} -e {preset.enabled}`
        noctalia.runAsync(cmd, function(result)
            if callback then callback(result.exitCode == 0) end
        end)
    else
        local cmd = `prisma-ctl profile apply {shellQuote(profileName)}`
        noctalia.runAsync(cmd, function(result)
            if callback then callback(result.exitCode == 0) end
        end)
    end
end

local function pollStatus(onComplete)
    if isPolling then return end
    isPolling = true

    noctalia.runAsync("prisma-ctl status --json", function(result)
        isPolling = false
        if result.exitCode == 0 and type(result.stdout) == "string" and result.stdout ~= "" then
            local ok, parsed = pcall(function()
                return noctalia.json.decode(result.stdout)
            end)

            if ok and type(parsed) == "table" then
                isAvailable = true

                -- Safely parse device state with fallback defaults
                local dev = parsed.device
                if type(dev) == "table" then
                    deviceState.enabled = if type(dev.enabled) == "number" then dev.enabled else 1
                    deviceState.saturation = if type(dev.saturation) == "number" then dev.saturation else 100
                    deviceState.contrast = if type(dev.contrast) == "number" then dev.contrast else 100
                    deviceState.brightness = if type(dev.brightness) == "number" then dev.brightness else 0

                    if not isDirty then
                        pendingState.enabled = deviceState.enabled
                        pendingState.saturation = deviceState.saturation
                        pendingState.contrast = deviceState.contrast
                        pendingState.brightness = deviceState.brightness
                    end
                end

                -- Safely parse hardware telemetry handling nulls
                local telem = parsed.telemetry
                if type(telem) == "table" then
                    telemetryState.card = if type(telem.card) == "string" then telem.card else "card1"
                    telemetryState.pci_id = if type(telem.pci_id) == "string" then telem.pci_id else ""
                    telemetryState.device_id = if type(telem.device_id) == "string" then telem.device_id else ""
                    telemetryState.gpu_busy_percent = if type(telem.gpu_busy_percent) == "number" then telem.gpu_busy_percent else 0
                    telemetryState.sclk_mhz = if type(telem.sclk_mhz) == "number" then telem.sclk_mhz else 0
                    telemetryState.vram_used_mib = if type(telem.vram_used_mib) == "number" then telem.vram_used_mib else 0
                    telemetryState.vram_total_mib = if type(telem.vram_total_mib) == "number" then telem.vram_total_mib else 0
                    telemetryState.temp_edge_c = if type(telem.temp_edge_c) == "number" then telem.temp_edge_c else 0
                    telemetryState.temp_junction_c = if type(telem.temp_junction_c) == "number" then telem.temp_junction_c else 0
                    telemetryState.power_watts = if type(telem.power_watts) == "number" then telem.power_watts else 0
                end

                appendHistory(historyGpu, telemetryState.gpu_busy_percent)
                appendHistory(historyTempJunc, telemetryState.temp_junction_c)
                appendHistory(historyPower, telemetryState.power_watts)
                appendHistory(historySclk, telemetryState.sclk_mhz)

                lastErrorMessage = ""
            else
                isAvailable = false
                lastErrorMessage = "Falha ao decodificar JSON de telemetria"
            end
        else
            isAvailable = false
            if type(result.stderr) == "string" and result.stderr ~= "" then
                lastErrorMessage = noctalia.string.trim(result.stderr)
            else
                lastErrorMessage = "Módulo chroma_amd não carregado ou permissão negada"
            end
        end

        publishStatus()
        if onComplete then onComplete() end
    end)
end

local function refreshProfiles()
    noctalia.runAsync("prisma-ctl profile list", function(result)
        local list = {}
        if result.exitCode == 0 and type(result.stdout) == "string" then
            for line in result.stdout:gmatch("[^\r\n]+") do
                local name, sat, con, bri, en = line:match("^%s*%-%s*([%w%-_]+)%s*%[sat=(%d+),%s*con=(%d+),%s*bri=([%-%d]+),%s*en=(%d+)%]")
                if name then
                    table.insert(list, {
                        name = name,
                        saturation = tonumber(sat) or 100,
                        contrast = tonumber(con) or 100,
                        brightness = tonumber(bri) or 0,
                        enabled = tonumber(en) or 1,
                    })
                end
            end
        end
        savedProfiles = list
        publishStatus()
    end)
end

local function onFocusChanged(newAppId)
    publishStatus()

    if not autoSwitchEnabled then
        return
    end

    local rule = appRules[newAppId]
    if rule and type(rule.profile) == "string" and rule.profile ~= "" then
        local targetProfile = rule.profile
        if activeAutoProfile ~= targetProfile then
            if activeAutoProfile == nil then
                -- Store desktop calibration before entering game profile
                fallbackState = {
                    saturation = deviceState.saturation,
                    contrast = deviceState.contrast,
                    brightness = deviceState.brightness,
                    enabled = deviceState.enabled,
                }
            end
            activeAutoProfile = targetProfile
            log(`Troca automática para perfil '{targetProfile}' (aplicativo: {newAppId})`)
            applyProfileByName(targetProfile, function()
                pollStatus()
            end)
        end
    else
        -- Unmapped app or desktop
        if activeAutoProfile ~= nil then
            log(`Saindo do modo de jogo ({activeAutoProfile}), restaurando calibragem anterior`)
            activeAutoProfile = nil
            if fallbackState ~= nil then
                local sat = fallbackState.saturation or 100
                local con = fallbackState.contrast or 100
                local bri = fallbackState.brightness or 0
                local en = fallbackState.enabled or 1
                fallbackState = nil
                local cmd = `prisma-ctl set -s {sat} -c {con} -b {bri} -e {en}`
                noctalia.runAsync(cmd, function()
                    pollStatus()
                end)
            else
                noctalia.runAsync("prisma-ctl profile reset", function()
                    pollStatus()
                end)
            end
        end
    end
end

local function pollWindows()
    if isQueryingWindows then return end
    isQueryingWindows = true

    noctalia.runAsync("niri msg --json windows", function(result)
        isQueryingWindows = false
        if result.exitCode == 0 and type(result.stdout) == "string" and result.stdout ~= "" then
            local ok, parsed = pcall(function()
                return noctalia.json.decode(result.stdout)
            end)

            if ok and type(parsed) == "table" then
                local detectedFocusedAppId = nil
                local detectedFocusedAppTitle = nil
                local appsMap = {}

                for _, win in ipairs(parsed) do
                    if type(win) == "table" and type(win.app_id) == "string" and win.app_id ~= "" then
                        local appId = win.app_id
                        local lowerId = string.lower(appId)
                        -- Filter internal shell and plugin components
                        if lowerId:find("noctalia") == nil and lowerId:find("prisma") == nil then
                            local title = if type(win.title) == "string" and win.title ~= "" then win.title else appId
                            local isFocused = (win.is_focused == true)

                            if isFocused then
                                detectedFocusedAppId = appId
                                detectedFocusedAppTitle = title
                            end

                            local existing = appsMap[appId]
                            if existing == nil then
                                appsMap[appId] = {
                                    app_id = appId,
                                    title = title,
                                    is_focused = isFocused,
                                }
                            else
                                if isFocused then
                                    existing.is_focused = true
                                    existing.title = title
                                end
                            end
                        end
                    end
                end

                local list = {}
                for _, item in pairs(appsMap) do
                    table.insert(list, item)
                end

                -- Sort: focused window first, then alphabetically by title / app_id
                table.sort(list, function(a, b)
                    if a.is_focused ~= b.is_focused then
                        return a.is_focused == true
                    end
                    local titleA = string.lower(a.title or a.app_id)
                    local titleB = string.lower(b.title or b.app_id)
                    return titleA < titleB
                end)

                openApps = list

                if detectedFocusedAppId ~= nil then
                    local changed = (detectedFocusedAppId ~= focusedAppId)
                    focusedAppId = detectedFocusedAppId
                    focusedAppTitle = if type(detectedFocusedAppTitle) == "string" and detectedFocusedAppTitle ~= ""
                        then detectedFocusedAppTitle
                        else detectedFocusedAppId

                    if changed then
                        onFocusChanged(detectedFocusedAppId)
                    else
                        publishStatus()
                    end
                else
                    if #list == 0 and activeAutoProfile ~= nil then
                        -- All windows closed, restore fallback state
                        log("Nenhuma janela aberta detectada, restaurando calibragem anterior")
                        activeAutoProfile = nil
                        if fallbackState ~= nil then
                            local sat = fallbackState.saturation or 100
                            local con = fallbackState.contrast or 100
                            local bri = fallbackState.brightness or 0
                            local en = fallbackState.enabled or 1
                            fallbackState = nil
                            local setCmd = `prisma-ctl set -s {sat} -c {con} -b {bri} -e {en}`
                            noctalia.runAsync(setCmd, function()
                                pollStatus()
                            end)
                        else
                            noctalia.runAsync("prisma-ctl profile reset", function()
                                pollStatus()
                            end)
                        end
                    else
                        publishStatus()
                    end
                end
            end
        end
    end)
end

local function flushSetCommand()
    if isMutating then return end
    isDirty = false
    isMutating = true

    local sat = pendingState.saturation
    local con = pendingState.contrast
    local bri = pendingState.brightness
    local en = pendingState.enabled

    local cmd = `prisma-ctl set -s {sat} -c {con} -b {bri} -e {en}`
    noctalia.runAsync(cmd, function(result)
        isMutating = false
        if result.exitCode == 0 then
            deviceState.saturation = sat
            deviceState.contrast = con
            deviceState.brightness = bri
            deviceState.enabled = en
            lastErrorMessage = ""
        else
            lastErrorMessage = noctalia.string.trim(result.stderr or result.stdout or "Falha ao aplicar parâmetros")
            log(`Erro ao aplicar comando set: {lastErrorMessage}`)
        end
        pollStatus()
    end)
end

local function handleCommand(cmd)
    if type(cmd) ~= "table" or type(cmd.action) ~= "string" then return end

    if cmd.action == "set" then
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

        if cmd.immediate then
            flushSetCommand()
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
                    lastErrorMessage = noctalia.string.trim(result.stderr or "Falha ao salvar perfil")
                    publishStatus()
                end
            end)
        end

    elseif cmd.action == "profile_apply" then
        local name = cmd.name
        if type(name) == "string" and name ~= "" then
            applyProfileByName(name, function(ok)
                if ok then
                    lastErrorMessage = ""
                    pollStatus()
                else
                    lastErrorMessage = "Falha ao aplicar perfil"
                    publishStatus()
                end
            end)
        end

    elseif cmd.action == "profile_delete" then
        local name = cmd.name
        if type(name) == "string" and name ~= "" then
            noctalia.runAsync(`prisma-ctl profile delete {shellQuote(name)}`, function(result)
                if result.exitCode == 0 then
                    lastErrorMessage = ""
                    refreshProfiles()
                else
                    lastErrorMessage = noctalia.string.trim(result.stderr or "Falha ao excluir perfil")
                    publishStatus()
                end
            end)
        end

    elseif cmd.action == "profile_reset" then
        activeAutoProfile = nil
        fallbackState = nil
        noctalia.runAsync("prisma-ctl profile reset", function(result)
            if result.exitCode == 0 then
                lastErrorMessage = ""
                pollStatus()
            else
                lastErrorMessage = noctalia.string.trim(result.stderr or "Falha ao resetar parâmetros")
                publishStatus()
            end
        end)

    elseif cmd.action == "profile_list" then
        refreshProfiles()

    elseif cmd.action == "panel_state" then
        panelOpen = if cmd.open then true else false
        if panelOpen then
            pollStatus()
            refreshProfiles()
            pollWindows()
        end

    elseif cmd.action == "toggle_auto_switch" then
        autoSwitchEnabled = not autoSwitchEnabled
        saveAppRules()
        if autoSwitchEnabled then
            if focusedAppId ~= "" then
                onFocusChanged(focusedAppId)
            end
        else
            if activeAutoProfile ~= nil then
                activeAutoProfile = nil
                if fallbackState ~= nil then
                    local sat = fallbackState.saturation or 100
                    local con = fallbackState.contrast or 100
                    local bri = fallbackState.brightness or 0
                    local en = fallbackState.enabled or 1
                    fallbackState = nil
                    local setCmd = `prisma-ctl set -s {sat} -c {con} -b {bri} -e {en}`
                    noctalia.runAsync(setCmd, function()
                        pollStatus()
                    end)
                end
            end
        end
        publishStatus()

    elseif cmd.action == "link_app_rule" then
        local appId = cmd.app_id
        local prof = cmd.profile
        if type(appId) == "string" and appId ~= "" and type(prof) == "string" and prof ~= "" then
            appRules[appId] = {
                profile = prof,
                title = if type(cmd.title) == "string" and cmd.title ~= "" then cmd.title else appId,
            }
            saveAppRules()
            if autoSwitchEnabled and (focusedAppId == appId or cmd.apply_now == true) then
                activeAutoProfile = nil -- Force re-evaluation
                onFocusChanged(appId)
            else
                publishStatus()
            end
        end

    elseif cmd.action == "unlink_app_rule" then
        local appId = cmd.app_id
        if type(appId) == "string" and appRules[appId] ~= nil then
            appRules[appId] = nil
            saveAppRules()
            if autoSwitchEnabled and focusedAppId == appId then
                onFocusChanged(appId)
            else
                publishStatus()
            end
        end
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

    -- Query open windows every 1s (20 ticks)
    if tickCount % TICKS_ACTIVE == 0 then
        pollWindows()
    end

    -- Periodic hardware telemetry polling interval
    local interval = if panelOpen then TICKS_ACTIVE else TICKS_IDLE
    if tickCount % interval == 0 then
        pollStatus()
    end
end

-- Listen for commands from the widget or interactive panel
noctalia.state.watch("command", handleCommand)

-- Initial probe, load saved app rules and profiles
loadAppRules()
pollStatus(function()
    refreshProfiles()
    pollWindows()
end)
