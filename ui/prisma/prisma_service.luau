--!nonstrict
-- Prisma Control Center - Headless Telemetry & Hardware Service
-- Manages background polling of AMD GPU hardware telemetry and color parameters,
-- maintains historical ring-buffers, debounces mutations, and handles profile management.

noctalia.setUpdateInterval(50)

local TICKS_ACTIVE = 20    -- 20 * 50ms = 1000ms (1s) when panel is open
local TICKS_IDLE = 100     -- 100 * 50ms = 5000ms (5s) in background
local DEBOUNCE_TICKS = 1   -- 1 * 50ms = 50ms debounce window for slider adjustments
local MAX_HISTORY = 60

local tickCount = 0
local panelOpen = false
local isPolling = false
local isMutating = false

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
}

-- Ring-buffers (up to 60 historical samples)
local historyGpu = {}
local historyTempJunc = {}
local historyPower = {}
local historySclk = {}

local savedProfiles = {}
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
        mutation_error_seq = mutationErrorSeq,
    })
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
                    if dev.available ~= nil then
                        isDeviceAvailable = (dev.available == true)
                    else
                        isDeviceAvailable = true
                    end
                    local hwEnabled = if type(dev.enabled) == "number" then dev.enabled else 1
                    local hwSat = if type(dev.saturation) == "number" then dev.saturation else 100
                    local hwCon = if type(dev.contrast) == "number" then dev.contrast else 100
                    local hwBri = if type(dev.brightness) == "number" then dev.brightness else 0

                    if not isDirty and not isMutating then
                        deviceState.enabled = hwEnabled
                        deviceState.saturation = hwSat
                        deviceState.contrast = hwCon
                        deviceState.brightness = hwBri

                        pendingState.enabled = hwEnabled
                        pendingState.saturation = hwSat
                        pendingState.contrast = hwCon
                        pendingState.brightness = hwBri
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
                end

                local effectiveTemp = if telemetryState.temp_junction_c > 0 then telemetryState.temp_junction_c else telemetryState.temp_edge_c

                appendHistory(historyGpu, telemetryState.gpu_busy_percent)
                appendHistory(historyTempJunc, effectiveTemp)
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

local function flushSetCommand()
    if isMutating then
        isDirty = true
        dirtyTicks = 0
        return
    end
    if not isDeviceAvailable then
        isDirty = false
        mutationErrorSeq += 1
        lastErrorMessage = "Módulo chroma_amd não disponível para gravação"
        publishStatus()
        return
    end
    isDirty = false
    isMutating = true

    local sat = pendingState.saturation
    local con = pendingState.contrast
    local bri = pendingState.brightness
    local en = pendingState.enabled

    publishStatus()

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
            mutationErrorSeq += 1
            if not isDirty then
                pendingState.saturation = deviceState.saturation
                pendingState.contrast = deviceState.contrast
                pendingState.brightness = deviceState.brightness
                pendingState.enabled = deviceState.enabled
            end
            lastErrorMessage = noctalia.string.trim(result.stderr or result.stdout or "Falha ao aplicar parâmetros")
            log(`Erro ao aplicar comando set: {lastErrorMessage}`)
        end
        publishStatus()
        if isDirty then
            flushSetCommand()
        else
            pollStatus()
        end
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
                    lastErrorMessage = noctalia.string.trim(result.stderr or "Falha ao salvar perfil")
                    publishStatus()
                end
            end)
        end

    elseif cmd.action == "profile_apply" then
        local name = cmd.name
        if type(name) == "string" and name ~= "" then
            noctalia.runAsync(`prisma-ctl profile apply {shellQuote(name)}`, function(result)
                if result.exitCode == 0 then
                    lastErrorMessage = ""
                    pollStatus()
                else
                    lastErrorMessage = noctalia.string.trim(result.stderr or "Falha ao aplicar perfil")
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

    -- Periodic polling interval
    local interval = if panelOpen then TICKS_ACTIVE else TICKS_IDLE
    if tickCount % interval == 0 then
        pollStatus()
    end
end

-- Listen for commands from the widget or interactive panel
noctalia.state.watch("command", handleCommand)

-- Initial probe and load
pollStatus(function()
    refreshProfiles()
end)
