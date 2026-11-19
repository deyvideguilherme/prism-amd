--!nonstrict
-- Prisma Control Center - Luau UI Behavioral Test Suite
-- Executes ui/prisma components in a deterministic mocked Noctalia runtime.

local function trim(s: string): string
    if type(s) ~= "string" then return "" end
    return string.match(s, "^%s*(.-)%s*$") or ""
end

local function decodeJson(str: string)
    local pos = 1
    local len = #str

    local function skipWhitespace()
        while pos <= len do
            local c = string.byte(str, pos)
            if c == 32 or c == 9 or c == 10 or c == 13 then
                pos += 1
            else
                break
            end
        end
    end

    local parseValue

    local function parseString(): string
        pos += 1
        local start = pos
        local result = ""
        while pos <= len do
            local c = string.byte(str, pos)
            if c == 92 then
                result ..= string.sub(str, start, pos - 1)
                pos += 1
                local esc = string.sub(str, pos, pos)
                if esc == '"' then result ..= '"'
                elseif esc == '\\' then result ..= '\\'
                elseif esc == '/' then result ..= '/'
                elseif esc == 'b' then result ..= '\b'
                elseif esc == 'f' then result ..= '\f'
                elseif esc == 'n' then result ..= '\n'
                elseif esc == 'r' then result ..= '\r'
                elseif esc == 't' then result ..= '\t'
                elseif esc == 'u' then
                    local hex = string.sub(str, pos + 1, pos + 4)
                    local cp = tonumber(hex, 16) or 0
                    result ..= utf8.char(cp)
                    pos += 4
                else
                    result ..= esc
                end
                pos += 1
                start = pos
            elseif c == 34 then
                result ..= string.sub(str, start, pos - 1)
                pos += 1
                return result
            else
                pos += 1
            end
        end
        error("Unterminated string in JSON")
    end

    local function parseNumber()
        local start = pos
        while pos <= len do
            local c = string.byte(str, pos)
            if (c >= 48 and c <= 57) or c == 45 or c == 43 or c == 46 or c == 101 or c == 69 then
                pos += 1
            else
                break
            end
        end
        local numStr = string.sub(str, start, pos - 1)
        local num = tonumber(numStr)
        if num == nil then
            error("Invalid number in JSON: " .. numStr)
        end
        return num
    end

    local function parseObject()
        pos += 1
        local obj = {}
        skipWhitespace()
        if pos <= len and string.byte(str, pos) == 125 then
            pos += 1
            return obj
        end

        while pos <= len do
            skipWhitespace()
            if string.byte(str, pos) ~= 34 then
                error("Expected string key in object at pos " .. pos)
            end
            local key = parseString()
            skipWhitespace()
            if string.byte(str, pos) ~= 58 then
                error("Expected ':' after key in object at pos " .. pos)
            end
            pos += 1
            local val = parseValue()
            if val ~= nil then
                obj[key] = val
            end
            skipWhitespace()
            local c = string.byte(str, pos)
            if c == 44 then
                pos += 1
            elseif c == 125 then
                pos += 1
                return obj
            else
                error("Expected ',' or '}' in object at pos " .. pos)
            end
        end
        error("Unterminated object in JSON")
    end

    local function parseArray()
        pos += 1
        local arr = {}
        skipWhitespace()
        if pos <= len and string.byte(str, pos) == 93 then
            pos += 1
            return arr
        end

        while pos <= len do
            local val = parseValue()
            table.insert(arr, val)
            skipWhitespace()
            local c = string.byte(str, pos)
            if c == 44 then
                pos += 1
            elseif c == 93 then
                pos += 1
                return arr
            else
                error("Expected ',' or ']' in array at pos " .. pos)
            end
        end
        error("Unterminated array in JSON")
    end

    function parseValue()
        skipWhitespace()
        if pos > len then
            error("Unexpected end of JSON input")
        end
        local c = string.byte(str, pos)
        if c == 34 then
            return parseString()
        elseif c == 123 then
            return parseObject()
        elseif c == 91 then
            return parseArray()
        elseif (c >= 48 and c <= 57) or c == 45 then
            return parseNumber()
        elseif string.sub(str, pos, pos + 3) == "true" then
            pos += 4
            return true
        elseif string.sub(str, pos, pos + 4) == "false" then
            pos += 5
            return false
        elseif string.sub(str, pos, pos + 3) == "null" then
            pos += 4
            return nil
        else
            error("Unexpected token in JSON at pos " .. pos)
        end
    end

    local result = parseValue()
    skipWhitespace()
    return result
end

-- Source loader:
-- 1. Try require("./ui_sources") dynamically provided by check_ui.py
-- 2. Try io.open if io library is available
-- 3. Fallback to embedded sources
local sources = nil
local okReq, resReq = pcall(function()
    return (require :: any)("./ui_sources")
end)
if not okReq then
    okReq, resReq = pcall(function()
        return (require :: any)("./tests/ui_sources")
    end)
end
if okReq and typeof(resReq) == "table" and resReq.service and resReq.widget and resReq.panel then
    sources = resReq
end

if sources == nil and typeof(io) == "table" and typeof(io.open) == "function" then
    local function readFile(p: string): string?
        local f = io.open(p, "r") or io.open("ui/prisma/" .. p, "r") or io.open("../ui/prisma/" .. p, "r")
        if f then
            local c = f:read("*a")
            f:close()
            return c
        end
        return nil
    end
    local s = readFile("prisma_service.luau")
    local w = readFile("prisma_widget.luau")
    local p = readFile("prisma_panel.luau")
    if s and w and p then
        sources = { service = s, widget = w, panel = p }
    end
end

if sources == nil then
    sources = {
        service = [==[--!nonstrict
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
        device = {
            enabled = currentDev.enabled,
            saturation = currentDev.saturation,
            contrast = currentDev.contrast,
            brightness = currentDev.brightness,
        },
        telemetry = {
            card = telemetryState.card,
            pci_id = telemetryState.pci_id,
            device_id = telemetryState.device_id,
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
                end

                -- Safely parse hardware telemetry handling nulls
                local telem = parsed.telemetry
                if type(telem) == "table" then
                    telemetryState.card = if type(telem.card) == "string" and telem.card ~= "" then telem.card else "amdgpu"
                    telemetryState.pci_id = if type(telem.pci_id) == "string" then telem.pci_id else ""
                    telemetryState.device_id = if type(telem.device_id) == "string" then telem.device_id else ""
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
    if isMutating then return end
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
            lastErrorMessage = noctalia.string.trim(result.stderr or result.stdout or "Falha ao aplicar parâmetros")
            log(`Erro ao aplicar comando set: {lastErrorMessage}`)
        end
        publishStatus()
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

        publishStatus()

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
]==],
        widget = [==[--!nonstrict
-- Prisma Control Center - Bar Widget Indicator
-- Displays real-time hardware status and selected telemetry metric on the Noctalia bar.
-- Clicking toggles the attached interactive Control Center panel.

local barMetric = noctalia.getConfig("bar_metric")
local hideInactive = noctalia.getConfig("hide_inactive")

local currentStatus = nil

local function updateDisplay()
    barMetric = noctalia.getConfig("bar_metric")
    hideInactive = noctalia.getConfig("hide_inactive")

    local status = currentStatus
    if type(status) ~= "table" or not status.available then
        barWidget.setGlyph("palette")
        barWidget.setGlyphColor("on_surface_variant")
        barWidget.setText("")
        barWidget.setTooltip("Prisma Control Center\nHardware não disponível ou módulo não carregado")
        barWidget.setVisible(not hideInactive)
        return
    end

    local dev = status.device or {}
    local telem = status.telemetry or {}

    local isEnabled = (dev.enabled == 1)
    if isEnabled then
        barWidget.setGlyph("palette")
        barWidget.setGlyphColor("primary")
    else
        barWidget.setGlyph("palette")
        barWidget.setGlyphColor("on_surface_variant")
    end

    local hasHotspot = (type(telem.temp_junction_c) == "number" and telem.temp_junction_c > 0)
    local tempJunc = if hasHotspot then telem.temp_junction_c else 0
    local tempEdge = if type(telem.temp_edge_c) == "number" then telem.temp_edge_c else 0
    local primaryTemp = if hasHotspot then tempJunc else tempEdge
    local primaryTempRounded = math.floor(primaryTemp + 0.5)

    local metricStr = ""
    local metricChoice = if type(barMetric) == "string" and barMetric ~= "" then barMetric else "hotspot"

    if metricChoice == "edge" then
        metricStr = `{math.floor(tempEdge + 0.5)}°C`
    elseif metricChoice == "gpu_busy" then
        local gpuBusy = if type(telem.gpu_busy_percent) == "number" then telem.gpu_busy_percent else 0
        metricStr = `{gpuBusy}%`
    else
        metricStr = `{primaryTempRounded}°C`
    end

    barWidget.setText(metricStr)

    local gpuBusy = if type(telem.gpu_busy_percent) == "number" then telem.gpu_busy_percent else 0
    local powerW = if type(telem.power_watts) == "number" then math.floor(telem.power_watts + 0.5) else 0
    local sat = if type(dev.saturation) == "number" then dev.saturation else 100
    local con = if type(dev.contrast) == "number" then dev.contrast else 100
    local bri = if type(dev.brightness) == "number" then dev.brightness else 0

    local tooltip = `Prisma Control Center\n`
        .. `GPU: {gpuBusy}% | {if hasHotspot then "Hotspot" else "Borda"}: {primaryTempRounded}°C | Consumo: {powerW}W\n`
        .. `Saturação: {sat}% | Contraste: {con}% | Brilho: {bri}%`

    barWidget.setTooltip(tooltip)
    barWidget.setVisible(true)
end

function onClick()
    noctalia.togglePanel("deyvideguilherme/prisma:panel")
end

function onConfigChanged()
    updateDisplay()
end

-- Subscribe to state updates published by prisma_service
noctalia.state.watch("status", function(newStatus)
    currentStatus = newStatus
    updateDisplay()
end)

-- Initial presentation
currentStatus = noctalia.state.get("status")
updateDisplay()
]==],
        panel = [==[--!nonstrict
-- Prisma Control Center - Interactive Desktop Panel
-- Visual control center featuring live hardware telemetry, DCN color calibration, and profile management.
-- Uses standard Noctalia native primitives (ui.box, ui.column, ui.row, ui.label, ui.button, ui.progress, ui.input, ui.scroll).

local currentTab = "performance" -- "performance" | "color" | "profiles"
local newProfileName = ""
local hasPendingColorUpdate = false

-- Cached local state to provide instant feedback before service roundtrips
local localDevice = {
    enabled = 1,
    saturation = 100,
    contrast = 100,
    brightness = 0,
}

local statusData = nil
local render

local function clamp(val, lo, hi)
    return math.max(lo, math.min(hi, val))
end

local function sendCommand(cmd)
    noctalia.state.set("command", cmd)
end

local function sendColorUpdate(immediate)
    hasPendingColorUpdate = true
    sendCommand({
        action = "set",
        enabled = localDevice.enabled,
        saturation = localDevice.saturation,
        contrast = localDevice.contrast,
        brightness = localDevice.brightness,
        immediate = immediate == true,
    })
end

-- ── Tab 1: Desempenho (Hardware Telemetry) ───────────────────────────────────

local function performanceView()
    local telem = (if statusData and statusData.telemetry then statusData.telemetry else {})

    local gpuBusy = if type(telem.gpu_busy_percent) == "number" then telem.gpu_busy_percent else 0
    local sclkMhz = if type(telem.sclk_mhz) == "number" then telem.sclk_mhz else 0

    local hasHotspot = (type(telem.temp_junction_c) == "number" and telem.temp_junction_c > 0)
    local tempJunc = if hasHotspot then math.floor(telem.temp_junction_c + 0.5) else 0
    local tempEdge = if type(telem.temp_edge_c) == "number" then math.floor(telem.temp_edge_c + 0.5) else 0
    local mainTemp = if hasHotspot then tempJunc else tempEdge
    local tempCeiling = if hasHotspot then 110 else 95
    local tempWarn = if hasHotspot then 85 else 80

    local isApu = (telem.is_apu == true)
    local powerW = if type(telem.power_watts) == "number" then math.floor(telem.power_watts + 0.5) else 0
    local powerMaxW = if isApu then 65 else 300

    local vramUsed = if type(telem.vram_used_mib) == "number" then telem.vram_used_mib else 0
    local vramTotal = if type(telem.vram_total_mib) == "number" and telem.vram_total_mib > 0 then telem.vram_total_mib else 1
    local vramRatio = clamp(vramUsed / vramTotal, 0, 1)

    local gttUsed = if type(telem.gtt_used_mib) == "number" then telem.gtt_used_mib else 0
    local gttTotal = if type(telem.gtt_total_mib) == "number" then telem.gtt_total_mib else 0

    local card = if type(telem.card) == "string" and telem.card ~= "" then telem.card else "AMD GPU"
    local pciId = if type(telem.pci_id) == "string" and telem.pci_id ~= "" then telem.pci_id else "AMD GPU"
    local devId = if type(telem.device_id) == "string" and telem.device_id ~= "" then telem.device_id else "DCN"

    -- 1. GPU Load & SCLK Clock Block
    local gpuBlock = ui.column({ gap = 4, align = "stretch" }, {
        ui.row({ align = "center", justify = "space_between" }, {
            ui.label({ text = "CARGA DA GPU", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.label({ text = `{sclkMhz} MHz`, fontSize = 12, color = "on_surface_variant" }),
        }),
        ui.row({ align = "baseline", gap = 6 }, {
            ui.label({ text = `{gpuBusy}%`, fontSize = 22, fontWeight = "bold", color = "primary" }),
            ui.label({ text = "Atividade de processamento gráfico", fontSize = 11, color = "on_surface_variant" }),
        }),
        ui.progress({
            height = 8,
            radius = 4,
            progress = clamp(gpuBusy / 100, 0, 1),
            fill = "primary",
        }),
    })

    -- 2. Temperatures Block (Hotspot & Edge)
    local tempRightLabel = if hasHotspot then `Borda: {tempEdge}°C` else "Sensor Único (Borda)"
    local tempSubtitle = if hasHotspot then "Temperatura interna máxima da GPU (Hotspot)" else "Temperatura do encapsulamento da APU (Edge)"
    local tempFill = if mainTemp > tempWarn then "error" else "secondary"

    local tempBlock = ui.column({ gap = 4, align = "stretch" }, {
        ui.row({ align = "center", justify = "space_between" }, {
            ui.label({ text = "TEMPERATURA", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.label({ text = tempRightLabel, fontSize = 12, color = "on_surface_variant" }),
        }),
        ui.row({ align = "baseline", gap = 6 }, {
            ui.label({
                text = `{mainTemp}°C`,
                fontSize = 22,
                fontWeight = "bold",
                color = tempFill,
            }),
            ui.label({ text = tempSubtitle, fontSize = 11, color = "on_surface_variant" }),
        }),
        ui.progress({
            height = 8,
            radius = 4,
            progress = clamp(mainTemp / tempCeiling, 0, 1),
            fill = tempFill,
        }),
    })

    -- 3. Power Consumption Block
    local powerSubtitle = if isApu then "Potência elétrica do pacote APU (PPT)" else "Potência elétrica da placa"

    local powerBlock = ui.column({ gap = 4, align = "stretch" }, {
        ui.row({ align = "center", justify = "space_between" }, {
            ui.label({ text = "CONSUMO DE ENERGIA", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.label({ text = `Teto: {powerMaxW} W`, fontSize = 12, color = "on_surface_variant" }),
        }),
        ui.row({ align = "baseline", gap = 6 }, {
            ui.label({ text = `{powerW} W`, fontSize = 22, fontWeight = "bold", color = "on_surface" }),
            ui.label({ text = powerSubtitle, fontSize = 11, color = "on_surface_variant" }),
        }),
        ui.progress({
            height = 8,
            radius = 4,
            progress = clamp(powerW / powerMaxW, 0, 1),
            fill = "primary",
        }),
    })

    -- 4. VRAM / Graphics Memory Allocation Block
    local vramTitle = "MEMÓRIA VRAM"
    local vramRightLabel = `{string.format("%.1f", vramUsed / 1024)} / {string.format("%.1f", vramTotal / 1024)} GiB`
    local vramPercent = `{math.floor(vramRatio * 100 + 0.5)}%`
    local vramSubtitle = `{math.floor(vramUsed)} MiB alocados`
    local vramProgressVal = vramRatio

    if isApu and gttTotal > 0 then
        vramTitle = "MEMÓRIA GRÁFICA (VRAM + GTT)"
        local combinedUsed = vramUsed + gttUsed
        local combinedTotal = vramTotal + gttTotal
        local memRatio = clamp(combinedUsed / combinedTotal, 0, 1)

        vramRightLabel = `{string.format("%.1f", combinedUsed / 1024)} / {string.format("%.1f", combinedTotal / 1024)} GiB`
        vramPercent = `{math.floor(memRatio * 100 + 0.5)}%`
        vramSubtitle = `{math.floor(vramUsed)}/{math.floor(vramTotal)} MiB VRAM + {math.floor(gttUsed)} MiB GTT`
        vramProgressVal = memRatio
    end

    local vramBlock = ui.column({ gap = 4, align = "stretch" }, {
        ui.row({ align = "center", justify = "space_between" }, {
            ui.label({ text = vramTitle, fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.label({
                text = vramRightLabel,
                fontSize = 12,
                color = "on_surface_variant",
            }),
        }),
        ui.row({ align = "baseline", gap = 6 }, {
            ui.label({ text = vramPercent, fontSize = 22, fontWeight = "bold", color = "primary" }),
            ui.label({ text = vramSubtitle, fontSize = 11, color = "on_surface_variant" }),
        }),
        ui.progress({
            height = 8,
            radius = 4,
            progress = vramProgressVal,
            fill = "primary",
        }),
    })

    -- Hardware Identity Footer Row
    local cardDisplay = `{card} ({if isApu then "APU" else "dGPU"})`
    local infoRow = ui.row({ align = "center", justify = "space_between" }, {
        ui.row({ gap = 12, align = "center" }, {
            ui.label({ text = cardDisplay, fontSize = 12, fontWeight = "bold", color = "on_surface" }),
            ui.label({ text = pciId, fontSize = 12, color = "on_surface_variant" }),
            ui.label({ text = devId, fontSize = 12, color = "on_surface_variant" }),
        }),
        ui.label({
            text = if statusData and statusData.available then "Hardware Ativo" else "Offline",
            fontSize = 12,
            fontWeight = "bold",
            color = if statusData and statusData.available then "primary" else "error",
        }),
    })

    return ui.column({ gap = 16, align = "stretch" }, {
        gpuBlock,
        tempBlock,
        powerBlock,
        vramBlock,
        infoRow,
    })
end

-- ── Tab 2: Tela & Cor (DCN Color Adjustments) ────────────────────────────────

local function interactiveBar(value, minVal, maxVal, isEnabled, onSelectValue)
    local segments = {}
    for i = 1, 25 do
        local targetVal = math.floor(minVal + ((i - 1) / 24) * (maxVal - minVal) + 0.5)
        local isFilled = (targetVal <= value)
        local fill = if isFilled then (if isEnabled then "primary" else "outline_variant") else "surface_container_highest"

        local onClick = function()
            if not isEnabled then
                return
            end
            onSelectValue(targetVal)
            sendColorUpdate(true)
            render()
        end

        table.insert(segments, ui.box({
            height = 14,
            flexGrow = 1,
            radius = 3,
            fill = fill,
            onClick = onClick,
        }))
    end
    return ui.row({ gap = 2, align = "center" }, segments)
end

local function colorView()
    local isEnabled = (localDevice.enabled == 1)

    -- 1. Master Enable Toggle Row
    local masterRow = ui.row({ align = "center", justify = "space_between" }, {
        ui.column({ gap = 2, flexGrow = 1 }, {
            ui.label({ text = "Calibragem de Cor em Hardware", fontSize = 14, fontWeight = "bold" }),
            ui.label({
                text = "Ajusta saturação, contraste e brilho diretamente na saída de vídeo da placa",
                fontSize = 11,
                color = "on_surface_variant",
            }),
        }),
        ui.button({
            text = if isEnabled then "Calibragem: ATIVADA" else "Calibragem: DESATIVADA",
            glyph = if isEnabled then "check" else "x",
            variant = if isEnabled then "primary" else "outline",
            onClick = "onToggleMaster",
        }),
    })

    -- 2. Precision Parameter Block with Interactive Bar & Direct Input
    local function parameterBlock(keyId, title, subtitle, value, minVal, maxVal, defaultVal, onSelectValue, onSub1, onAdd1, onReset)
        local onSubmitInput = function(text)
            if not isEnabled then
                return
            end
            local trimmed = noctalia.string.trim(text)
            if string.sub(trimmed, -1) == "%" then
                trimmed = noctalia.string.trim(string.sub(trimmed, 1, -2))
            end
            local num = tonumber(trimmed)
            if num ~= nil then
                local clamped = clamp(math.floor(num + 0.5), minVal, maxVal)
                onSelectValue(clamped)
                sendColorUpdate(true)
                render()
            else
                render()
            end
        end

        local headerRow = ui.row({ align = "center", justify = "space_between" }, {
            ui.column({ gap = 2, flexGrow = 1 }, {
                ui.label({ text = title, fontSize = 13, fontWeight = "bold" }),
                ui.label({ text = subtitle, fontSize = 11, color = "on_surface_variant" }),
            }),
            ui.row({ gap = 6, align = "center" }, {
                ui.label({ text = `{value}%`, fontSize = 15, fontWeight = "bold", color = "primary" }),
                ui.input({
                    key = `input_{keyId}_{value}`,
                    placeholder = tostring(value),
                    width = 64,
                    onSubmit = onSubmitInput,
                }),
                ui.button({
                    text = "-1",
                    variant = "ghost",
                    enabled = isEnabled and value > minVal,
                    onClick = onSub1,
                }),
                ui.button({
                    text = "+1",
                    variant = "ghost",
                    enabled = isEnabled and value < maxVal,
                    onClick = onAdd1,
                }),
                ui.button({
                    glyph = "rotate",
                    text = `{defaultVal}%`,
                    variant = "outline",
                    tooltip = `Restaurar padrão ({defaultVal}%)`,
                    enabled = isEnabled and value ~= defaultVal,
                    onClick = onReset,
                }),
            }),
        })

        local bar = interactiveBar(value, minVal, maxVal, isEnabled, onSelectValue)

        return ui.column({ gap = 8, align = "stretch" }, {
            headerRow,
            bar,
        })
    end

    local satBlock = parameterBlock(
        "saturation",
        "Saturação",
        "Intensidade e vivacidade das cores na tela (0..400%)",
        localDevice.saturation,
        0, 400, 100,
        function(val) localDevice.saturation = clamp(val, 0, 400) end,
        "onSaturationSub1",
        "onSaturationAdd1",
        "onSaturationReset"
    )

    local conBlock = parameterBlock(
        "contrast",
        "Contraste",
        "Diferença entre tons claros e escuros (10..300%)",
        localDevice.contrast,
        10, 300, 100,
        function(val) localDevice.contrast = clamp(val, 10, 300) end,
        "onContrastSub1",
        "onContrastAdd1",
        "onContrastReset"
    )

    local briBlock = parameterBlock(
        "brightness",
        "Brilho",
        "Nível geral de luminosidade da imagem (-100..100%)",
        localDevice.brightness,
        -100, 100, 0,
        function(val) localDevice.brightness = clamp(val, -100, 100) end,
        "onBrightnessSub1",
        "onBrightnessAdd1",
        "onBrightnessReset"
    )

    -- 3. Presets Rápidos
    local presetsBlock = ui.column({ gap = 8, align = "stretch" }, {
        ui.label({ text = "PRESETS RÁPIDOS", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
        ui.row({ gap = 8, align = "center" }, {
            ui.button({
                text = "Padrão",
                glyph = "rotate",
                variant = "outline",
                enabled = isEnabled,
                onClick = "onPresetStandard",
                flexGrow = 1,
            }),
            ui.button({
                text = "Competitivo",
                glyph = "device-gamepad-2",
                variant = "outline",
                enabled = isEnabled,
                onClick = "onPresetCompetitive",
                flexGrow = 1,
            }),
            ui.button({
                text = "Cinema",
                glyph = "movie",
                variant = "outline",
                enabled = isEnabled,
                onClick = "onPresetCinema",
                flexGrow = 1,
            }),
            ui.button({
                text = "Monocromático",
                glyph = "palette",
                variant = "outline",
                enabled = isEnabled,
                onClick = "onPresetMono",
                flexGrow = 1,
            }),
        }),
    })

    return ui.column({ gap = 16, align = "stretch" }, {
        masterRow,
        satBlock,
        conBlock,
        briBlock,
        presetsBlock,
    })
end

-- ── Tab 3: Perfis (Profiles Management) ──────────────────────────────────────

local function profilesView()
    local profiles = if statusData and statusData.profiles then statusData.profiles else {}

    -- Save Profile Block
    local saveBlock = ui.column({ gap = 8, align = "stretch" }, {
        ui.label({ text = "SALVAR ESTADO ATUAL", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
        ui.row({ gap = 8, align = "center" }, {
            ui.input({
                key = "profile_input",
                placeholder = "Nome do perfil (ex: jogos, filmes)",
                value = newProfileName,
                flexGrow = 1,
                onChange = "onProfileNameChange",
                onSubmit = "onSaveProfileSubmit",
            }),
            ui.button({
                text = "Salvar",
                glyph = "device-floppy",
                variant = "primary",
                enabled = (newProfileName ~= ""),
                onClick = "onSaveProfileClicked",
            }),
        }),
    })

    -- Profile List Items
    local profileRows = {}
    if #profiles == 0 then
        table.insert(profileRows, ui.label({
            text = "Nenhum perfil salvo ainda",
            color = "on_surface_variant",
            fontSize = 12,
        }))
    else
        for _, p in ipairs(profiles) do
            local name = p.name or "perfil"
            local sat = p.saturation or 100
            local con = p.contrast or 100
            local bri = p.brightness or 0

            local onApply = function()
                hasPendingColorUpdate = false
                sendCommand({ action = "profile_apply", name = name })
            end

            local onDelete = function()
                sendCommand({ action = "profile_delete", name = name })
            end

            table.insert(profileRows, ui.row({ align = "center", justify = "space_between" }, {
                ui.column({ gap = 2, flexGrow = 1 }, {
                    ui.label({ text = name, fontSize = 14, fontWeight = "bold" }),
                    ui.label({
                        text = `Saturação: {sat}% | Contraste: {con}% | Brilho: {bri}%`,
                        fontSize = 11,
                        color = "on_surface_variant",
                    }),
                }),
                ui.row({ gap = 6, align = "center" }, {
                    ui.button({
                        text = "Aplicar",
                        glyph = "check",
                        variant = "primary",
                        onClick = onApply,
                    }),
                    ui.button({
                        glyph = "trash",
                        variant = "destructive",
                        tooltip = `Excluir perfil '{name}'`,
                        onClick = onDelete,
                    }),
                }),
            }))
        end
    end

    local listBlock = ui.column({ gap = 8, align = "stretch" }, {
        ui.label({ text = "PERFIS SALVOS", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
        ui.column({ gap = 10, align = "stretch" }, profileRows),
    })

    -- Profile Actions Footer
    local actionsRow = ui.row({ align = "center", justify = "space_between" }, {
        ui.button({
            glyph = "refresh",
            text = "Recarregar Lista",
            variant = "ghost",
            onClick = "onRefreshProfilesClicked",
        }),
        ui.button({
            text = "Restaurar Padrões",
            glyph = "rotate",
            variant = "destructive",
            onClick = "onResetDefaultsClicked",
        }),
    })

    return ui.column({ gap = 16, align = "stretch" }, {
        saveBlock,
        listBlock,
        actionsRow,
    })
end

-- ── Main Panel Render ────────────────────────────────────────────────────────

render = function()
    -- Sync local state with status
    if statusData and statusData.device then
        local dev = statusData.device
        if not hasPendingColorUpdate then
            localDevice.enabled = if type(dev.enabled) == "number" then dev.enabled else 1
            localDevice.saturation = if type(dev.saturation) == "number" then dev.saturation else 100
            localDevice.contrast = if type(dev.contrast) == "number" then dev.contrast else 100
            localDevice.brightness = if type(dev.brightness) == "number" then dev.brightness else 0
        elseif dev.saturation == localDevice.saturation and
               dev.contrast == localDevice.contrast and
               dev.brightness == localDevice.brightness and
               dev.enabled == localDevice.enabled then
            hasPendingColorUpdate = false
        end
    end

    -- Header row
    local header = ui.row({ align = "center", justify = "space_between" }, {
        ui.row({ gap = 8, align = "center" }, {
            ui.label({ text = "Prisma Control Center", fontSize = 16, fontWeight = "bold", color = "on_surface" }),
        }),
        ui.button({ glyph = "x", variant = "ghost", tooltip = "Fechar", onClick = "onCloseClicked" }),
    })

    -- Navigation tab buttons
    local nav = ui.row({ gap = 8, align = "center" }, {
        ui.button({
            text = "Desempenho",
            glyph = "chart-line",
            variant = if currentTab == "performance" then "primary" else "ghost",
            onClick = "onTabPerformance",
            flexGrow = 1,
        }),
        ui.button({
            text = "Tela & Cor",
            glyph = "adjustments",
            variant = if currentTab == "color" then "primary" else "ghost",
            onClick = "onTabColor",
            flexGrow = 1,
        }),
        ui.button({
            text = "Perfis",
            glyph = "palette",
            variant = if currentTab == "profiles" then "primary" else "ghost",
            onClick = "onTabProfiles",
            flexGrow = 1,
        }),
    })

    -- Error banner if present
    local banner = nil
    if statusData and type(statusData.error) == "string" and statusData.error ~= "" then
        banner = ui.row({ gap = 8, align = "center" }, {
            ui.label({ text = `! {statusData.error}`, color = "error", fontSize = 12 }),
        })
    end

    -- Body content based on active tab
    local tabContent
    if currentTab == "color" then
        tabContent = colorView()
    elseif currentTab == "profiles" then
        tabContent = profilesView()
    else
        tabContent = performanceView()
    end

    local children = { header, nav }
    if banner ~= nil then
        table.insert(children, banner)
    end
    table.insert(children, ui.scroll({ flexGrow = 1, gap = 12 }, { tabContent }))

    panel.render(ui.column({ flexGrow = 1, gap = 14, align = "stretch" }, children))
end

-- ── Event Callbacks ──────────────────────────────────────────────────────────

function onTabPerformance()
    currentTab = "performance"
    render()
end

function onTabColor()
    currentTab = "color"
    render()
end

function onTabProfiles()
    currentTab = "profiles"
    sendCommand({ action = "profile_list" })
    render()
end

function onCloseClicked()
    sendCommand({ action = "panel_state", open = false })
    panel.close()
end

function onToggleMaster()
    localDevice.enabled = if localDevice.enabled == 1 then 0 else 1
    sendColorUpdate(true)
    render()
end

function onSaturationSub1()
    localDevice.saturation = clamp(localDevice.saturation - 1, 0, 400)
    sendColorUpdate(true)
    render()
end

function onSaturationAdd1()
    localDevice.saturation = clamp(localDevice.saturation + 1, 0, 400)
    sendColorUpdate(true)
    render()
end

function onSaturationReset()
    localDevice.saturation = 100
    sendColorUpdate(true)
    render()
end

function onContrastSub1()
    localDevice.contrast = clamp(localDevice.contrast - 1, 10, 300)
    sendColorUpdate(true)
    render()
end

function onContrastAdd1()
    localDevice.contrast = clamp(localDevice.contrast + 1, 10, 300)
    sendColorUpdate(true)
    render()
end

function onContrastReset()
    localDevice.contrast = 100
    sendColorUpdate(true)
    render()
end

function onBrightnessSub1()
    localDevice.brightness = clamp(localDevice.brightness - 1, -100, 100)
    sendColorUpdate(true)
    render()
end

function onBrightnessAdd1()
    localDevice.brightness = clamp(localDevice.brightness + 1, -100, 100)
    sendColorUpdate(true)
    render()
end

function onBrightnessReset()
    localDevice.brightness = 0
    sendColorUpdate(true)
    render()
end

function onPresetStandard()
    localDevice.saturation = 100
    localDevice.contrast = 100
    localDevice.brightness = 0
    localDevice.enabled = 1
    sendColorUpdate(true)
    render()
end

function onPresetCompetitive()
    localDevice.saturation = 160
    localDevice.contrast = 115
    localDevice.brightness = 10
    localDevice.enabled = 1
    sendColorUpdate(true)
    render()
end

function onPresetCinema()
    localDevice.saturation = 120
    localDevice.contrast = 105
    localDevice.brightness = -5
    localDevice.enabled = 1
    sendColorUpdate(true)
    render()
end

function onPresetMono()
    localDevice.saturation = 0
    localDevice.contrast = 110
    localDevice.brightness = 0
    localDevice.enabled = 1
    sendColorUpdate(true)
    render()
end

function onProfileNameChange(text)
    newProfileName = noctalia.string.trim(text)
end

function onSaveProfileSubmit(text)
    local name = noctalia.string.trim(text)
    if name ~= "" then
        sendCommand({ action = "profile_save", name = name })
        newProfileName = ""
        render()
    end
end

function onSaveProfileClicked()
    if newProfileName ~= "" then
        sendCommand({ action = "profile_save", name = newProfileName })
        newProfileName = ""
        render()
    end
end

function onRefreshProfilesClicked()
    sendCommand({ action = "profile_list" })
end

function onResetDefaultsClicked()
    hasPendingColorUpdate = false
    sendCommand({ action = "profile_reset" })
end

-- Subscribe to telemetry/state updates
noctalia.state.watch("status", function(newStatus)
    statusData = newStatus
    render()
end)

-- Notify service that panel is open
sendCommand({ action = "panel_state", open = true })

-- Initial render
statusData = noctalia.state.get("status")
render()
]==],
    }
end

assert(sources ~= nil and sources.service and sources.widget and sources.panel, "Failed to obtain UI component sources")

-- Tree inspection helpers
local function findNodes(tree, predicate, acc)
    acc = acc or {}
    if type(tree) ~= "table" then return acc end
    if predicate(tree) then
        table.insert(acc, tree)
    end
    if type(tree.children) == "table" then
        for _, child in ipairs(tree.children) do
            findNodes(child, predicate, acc)
        end
    end
    return acc
end

local function hasLabelWithText(tree, substring)
    local labels = findNodes(tree, function(n)
        return n.type == "label" and type(n.props.text) == "string" and string.find(n.props.text, substring, 1, true) ~= nil
    end)
    return #labels > 0
end

local function findProgressBars(tree)
    return findNodes(tree, function(n)
        return n.type == "progress" and type(n.props.progress) == "number"
    end)
end

-- Mock Environment & Noctalia APIs
local mockConfig = {
    bar_metric = "hotspot",
    hide_inactive = false,
}

local mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","pci_id":"0000:07:00.0","device_id":"1002:1638","is_apu":true,"gpu_busy_percent":2,"sclk_mhz":400,"vram_used_mib":455.3,"vram_total_mib":512.0,"gtt_used_mib":643.9,"gtt_total_mib":7649.9,"temp_edge_c":34.0,"temp_junction_c":null,"power_watts":25.0}}]],
    stderr = "",
}

local mockBar = {
    glyph = "",
    glyphColor = "",
    text = "",
    tooltip = "",
    visible = true,
}

local mockPanel = {
    tree = nil,
    closed = false,
}

local mockState = {}
local stateWatchers = {}

local pendingAsyncCallbacks = {}
local deferSetCommands = false

local noctalia = {
    setUpdateInterval = function(intervalMs) end,
    log = function(msg) end,
    getConfig = function(key)
        return mockConfig[key]
    end,
    togglePanel = function(panelId) end,
    string = {
        trim = trim,
    },
    json = {
        decode = decodeJson,
    },
    runAsync = function(cmd, cb)
        if string.find(cmd, "prisma-ctl status --json", 1, true) then
            cb(mockStatusResponse)
        elseif string.find(cmd, "prisma-ctl profile list", 1, true) then
            cb({ exitCode = 0, stdout = "", stderr = "" })
        elseif string.find(cmd, "prisma-ctl set", 1, true) then
            if deferSetCommands then
                table.insert(pendingAsyncCallbacks, { cmd = cmd, cb = cb })
            else
                cb({ exitCode = 0, stdout = "", stderr = "" })
            end
        else
            cb({ exitCode = 0, stdout = "", stderr = "" })
        end
    end,
    state = {
        get = function(key)
            return mockState[key]
        end,
        set = function(key, val)
            mockState[key] = val
            if stateWatchers[key] then
                for _, cb in ipairs(stateWatchers[key]) do
                    cb(val)
                end
            end
        end,
        watch = function(key, cb)
            if not stateWatchers[key] then
                stateWatchers[key] = {}
            end
            table.insert(stateWatchers[key], cb)
        end,
    },
}

local barWidget = {
    setGlyph = function(glyph) mockBar.glyph = glyph end,
    setGlyphColor = function(color) mockBar.glyphColor = color end,
    setText = function(text) mockBar.text = text end,
    setTooltip = function(tooltip) mockBar.tooltip = tooltip end,
    setVisible = function(visible) mockBar.visible = visible end,
}

local ui = {
    column = function(props, children) return { type = "column", props = props or {}, children = children or {} } end,
    row = function(props, children) return { type = "row", props = props or {}, children = children or {} } end,
    box = function(props, children) return { type = "box", props = props or {}, children = children or {} } end,
    scroll = function(props, children) return { type = "scroll", props = props or {}, children = children or {} } end,
    label = function(props) return { type = "label", props = props or {}, children = {} } end,
    progress = function(props) return { type = "progress", props = props or {}, children = {} } end,
    button = function(props) return { type = "button", props = props or {}, children = {} } end,
    input = function(props) return { type = "input", props = props or {}, children = {} } end,
}

local panel = {
    render = function(tree) mockPanel.tree = tree end,
    close = function() mockPanel.closed = true end,
}

local mockEnv = {
    noctalia = noctalia,
    barWidget = barWidget,
    ui = ui,
    panel = panel,
}
setmetatable(mockEnv, { __index = getfenv() })

-- Execute service, widget, and panel in the shared environment
local serviceFn = loadstring(sources.service)
assert(serviceFn ~= nil, "Failed to compile prisma_service.luau")
setfenv(serviceFn, mockEnv)
serviceFn()

local widgetFn = loadstring(sources.widget)
assert(widgetFn ~= nil, "Failed to compile prisma_widget.luau")
setfenv(widgetFn, mockEnv)
widgetFn()

local panelFn = loadstring(sources.panel)
assert(panelFn ~= nil, "Failed to compile prisma_panel.luau")
setfenv(panelFn, mockEnv)
panelFn()

print("=== Prisma Noctalia UI Behavioral Test Suite ===")

-- Cenário 1 — APU (Ryzen 5 5600GT)
print("[test_ui] Running Scenario 1: APU (Ryzen 5 5600GT)...")
for _ = 1, 20 do
    mockEnv.update()
end

local st1 = mockState["status"]
assert(st1 ~= nil, "Scenario 1: mockState['status'] must not be nil")
assert(st1.telemetry.is_apu == true, "Scenario 1: telemetry.is_apu must be true")
local hist1 = st1.history.temp_junction
assert(hist1 ~= nil and #hist1 > 0, "Scenario 1: temp_junction history must not be empty")
assert(hist1[#hist1] == 34.0, "Scenario 1: temp_junction history must record 34.0 (fallback), got: " .. tostring(hist1[#hist1]))

assert(mockBar.text == "34°C", "Scenario 1: mockBar.text must be 34°C, got: " .. tostring(mockBar.text))
assert(string.find(mockBar.tooltip, "Borda: 34°C", 1, true) ~= nil, "Scenario 1: tooltip must contain 'Borda: 34°C', got: " .. tostring(mockBar.tooltip))
assert(string.find(mockBar.tooltip, "Hotspot", 1, true) == nil, "Scenario 1: tooltip must not contain 'Hotspot'")
assert(string.find(mockBar.tooltip, "0°C", 1, true) == nil, "Scenario 1: tooltip must not contain '0°C'")

local tree1 = mockPanel.tree
assert(tree1 ~= nil, "Scenario 1: mockPanel.tree must not be nil")
assert(hasLabelWithText(tree1, "MEMÓRIA GRÁFICA (VRAM + GTT)"), "Scenario 1: tree must contain label 'MEMÓRIA GRÁFICA (VRAM + GTT)'")
assert(hasLabelWithText(tree1, "Teto: 65 W"), "Scenario 1: tree must contain label 'Teto: 65 W'")
assert(hasLabelWithText(tree1, "Sensor Único (Borda)"), "Scenario 1: tree must contain label 'Sensor Único (Borda)'")
assert(hasLabelWithText(tree1, "card1 (APU)"), "Scenario 1: tree must contain label 'card1 (APU)'")
assert(hasLabelWithText(tree1, "455/512 MiB VRAM + 643 MiB GTT"), "Scenario 1: tree must contain label '455/512 MiB VRAM + 643 MiB GTT'")

local pbars1 = findProgressBars(tree1)
local expPowerRatio1 = 25 / 65
local expMemRatio1 = (455.3 + 643.9) / (512.0 + 7649.9)
local foundPower1 = false
local foundMem1 = false
for _, p in ipairs(pbars1) do
    if math.abs(p.props.progress - expPowerRatio1) < 0.005 then foundPower1 = true end
    if math.abs(p.props.progress - expMemRatio1) < 0.005 then foundMem1 = true end
end
assert(foundPower1, "Scenario 1: progress bar for power 25/65 not found")
assert(foundMem1, "Scenario 1: progress bar for memory combined ratio not found")
print("  [PASS] Scenario 1 passed.")

-- Cenário 2 — dGPU (RX 9060 XT)
print("[test_ui] Running Scenario 2: dGPU (RX 9060 XT)...")
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","pci_id":"0000:2d:00.0","device_id":"1002:7590","is_apu":false,"gpu_busy_percent":7,"sclk_mhz":964,"vram_used_mib":2048.0,"vram_total_mib":8192.0,"gtt_used_mib":412.0,"gtt_total_mib":16000.0,"temp_edge_c":54.0,"temp_junction_c":68.5,"power_watts":85.5}}]],
    stderr = "",
}
for _ = 1, 20 do
    mockEnv.update()
end

assert(mockBar.text == "69°C", "Scenario 2: mockBar.text must be 69°C (rounded from 68.5°C), got: " .. tostring(mockBar.text))
assert(string.find(mockBar.tooltip, "Hotspot: 69°C", 1, true) ~= nil, "Scenario 2: tooltip must contain 'Hotspot: 69°C', got: " .. tostring(mockBar.tooltip))

local tree2 = mockPanel.tree
assert(tree2 ~= nil, "Scenario 2: mockPanel.tree must not be nil")
assert(hasLabelWithText(tree2, "MEMÓRIA VRAM"), "Scenario 2: tree must contain label 'MEMÓRIA VRAM'")
assert(hasLabelWithText(tree2, "Teto: 300 W"), "Scenario 2: tree must contain label 'Teto: 300 W'")
assert(hasLabelWithText(tree2, "Borda: 54°C"), "Scenario 2: tree must contain label 'Borda: 54°C'")
assert(hasLabelWithText(tree2, "card1 (dGPU)"), "Scenario 2: tree must contain label 'card1 (dGPU)'")

local pbars2 = findProgressBars(tree2)
local expPowerRatio2 = 86 / 300
local foundPower2 = false
for _, p in ipairs(pbars2) do
    if math.abs(p.props.progress - expPowerRatio2) < 0.005 then foundPower2 = true end
end
assert(foundPower2, "Scenario 2: progress bar for power 86/300 not found")
print("  [PASS] Scenario 2 passed.")

-- Cenário 3 — Hardware offline (exitCode = 1)
print("[test_ui] Running Scenario 3: Hardware offline...")
mockConfig.hide_inactive = true
mockStatusResponse = {
    exitCode = 1,
    stdout = "",
    stderr = "Device not found",
}
for _ = 1, 20 do
    mockEnv.update()
end
mockEnv.onConfigChanged()

assert(mockBar.visible == false, "Scenario 3: mockBar.visible must be false when hide_inactive=true and hardware offline")
assert(mockBar.text == "", "Scenario 3: mockBar.text must be empty when hardware offline, got: " .. tostring(mockBar.text))
print("  [PASS] Scenario 3 passed.")

-- Cenário 4 — Ajuste Otimista de Cor e Imunidade a Snapshots Obsoletos
print("[test_ui] Running Scenario 4: Optimistic color update and snapshot immunity...")
mockConfig.hide_inactive = false
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","pci_id":"0000:07:00.0","device_id":"1002:1638","is_apu":true,"gpu_busy_percent":2,"sclk_mhz":400,"vram_used_mib":455.3,"vram_total_mib":512.0,"gtt_used_mib":643.9,"gtt_total_mib":7649.9,"temp_edge_c":34.0,"temp_junction_c":null,"power_watts":25.0}}]],
    stderr = "",
}
for _ = 1, 20 do
    mockEnv.update()
end
mockEnv.onConfigChanged()

-- Navegar para a aba de Tela & Cor
mockEnv.onTabColor()
local treeColor = mockPanel.tree
assert(treeColor ~= nil, "Scenario 4: mockPanel.tree must not be nil")
assert(hasLabelWithText(treeColor, "Calibragem de Cor em Hardware"), "Scenario 4: must navigate to 'Tela & Cor'")
assert(hasLabelWithText(treeColor, "100%"), "Scenario 4: initial saturation must be 100%")

-- Localizar os segmentos interativos do slider
local segmentBoxes = findNodes(treeColor, function(n)
    return n.type == "box" and type(n.props.onClick) == "function"
end)
assert(#segmentBoxes == 75, "Scenario 4: expected 75 interactive segment boxes across 3 parameters, found: " .. tostring(#segmentBoxes))

-- Habilitar retenção assíncrona do comando prisma-ctl set
deferSetCommands = true

-- Clicar no segmento 10 da barra de saturação (targetVal = 150)
segmentBoxes[10].props.onClick()

-- Verificar atualização otimista imediata na árvore do painel (150%)
local treeOptimistic = mockPanel.tree
assert(hasLabelWithText(treeOptimistic, "150%"), "Scenario 4: panel must immediately show 150% without waiting for hardware")

-- Verificar publicação otimista no estado global emitido pelo serviço
local statusOptimistic = mockState["status"]
assert(statusOptimistic ~= nil and statusOptimistic.device ~= nil, "Scenario 4: status must be published")
assert(statusOptimistic.device.saturation == 150, "Scenario 4: status.device.saturation must be 150 immediately")

-- Verificar que comando prisma-ctl set está em trânsito (in-flight)
assert(#pendingAsyncCallbacks == 1, "Scenario 4: prisma-ctl set command must be in-flight")
assert(string.find(pendingAsyncCallbacks[1].cmd, "prisma-ctl set -s 150", 1, true) ~= nil, "Scenario 4: command must set saturation to 150")

-- Simular múltiplos ciclos de poll de background enquanto a mutação está em trânsito
-- O hardware ainda responde o snapshot obsoleto com 100% de saturação
for _ = 1, 10 do
    mockEnv.update()
end

-- Assegurar que painel e status MANTÊM 150% (SEM SNAP-BACK para 100%!)
local treeInFlight = mockPanel.tree
assert(hasLabelWithText(treeInFlight, "150%"), "Scenario 4: panel must retain 150% without snap-back while mutation is in flight")
local statusInFlight = mockState["status"]
assert(statusInFlight.device.saturation == 150, "Scenario 4: status device.saturation must remain 150 while mutating")

-- Concluir a mutação em hardware e atualizar resposta do mock
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"enabled":1,"saturation":150,"contrast":100,"brightness":0},"telemetry":{"card":"card1","pci_id":"0000:07:00.0","device_id":"1002:1638","is_apu":true,"gpu_busy_percent":2,"sclk_mhz":400,"vram_used_mib":455.3,"vram_total_mib":512.0,"gtt_used_mib":643.9,"gtt_total_mib":7649.9,"temp_edge_c":34.0,"temp_junction_c":null,"power_watts":25.0}}]],
    stderr = "",
}

local inFlightCmd = table.remove(pendingAsyncCallbacks, 1)
deferSetCommands = false
inFlightCmd.cb({ exitCode = 0, stdout = "", stderr = "" })

-- Verificar reconciliação final após conclusão do hardware
for _ = 1, 5 do
    mockEnv.update()
end
assert(hasLabelWithText(mockPanel.tree, "150%"), "Scenario 4: panel must remain at 150% after hardware completion")
assert(mockState["status"].device.saturation == 150, "Scenario 4: status must be 150 after hardware completion")
print("  [PASS] Scenario 4 passed.")

print("All Luau UI behavioral tests PASSED successfully!")
