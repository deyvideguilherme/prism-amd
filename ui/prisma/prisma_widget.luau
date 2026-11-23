--!nonstrict
-- Prisma Control Center - Bar Widget Indicator
-- Displays real-time hardware status and selected telemetry metric on the Noctalia bar.
-- Clicking toggles the attached interactive Control Center panel.

local currentStatus = nil

local function tr(key, fallback, subst)
    if type(noctalia.tr) == "function" then
        local ok, value = pcall(noctalia.tr, key, subst)
        if ok and type(value) == "string" and value ~= "" and value ~= key then
            return value
        end
    end
    if subst then
        return (fallback:gsub("{(%w+)}", function(name) return tostring(subst[name]) end))
    end
    return fallback
end

local function calibrationAvailable(status)
    return type(status) == "table" and status.available and status.device and status.device.available == true
end

local function updateDisplay()
    local status = if type(currentStatus) == "table" then currentStatus else {}
    local dev = status.device or {}
    local telem = status.telemetry or {}
    local available = calibrationAvailable(status)
    local enabled = available and dev.enabled == 1
    local tooltip = { tr("name", "Prisma Control Center") }

    if enabled then
        barWidget.setGlyph("palette")
        barWidget.setGlyphColor("primary")
        table.insert(tooltip, tr("panel.color.enabled", "Calibragem: ATIVADA"))
    elseif available then
        barWidget.setGlyph("player-pause")
        barWidget.setGlyphColor("on_surface_variant")
        table.insert(tooltip, tr("panel.color.disabled", "Calibragem: DESATIVADA"))
    else
        barWidget.setGlyph("plug-off")
        barWidget.setGlyphColor("error")
        table.insert(tooltip, tr("widget.offline", "Hardware não disponível ou módulo não carregado"))
    end

    local function reading(key, unit)
        if status.available and telem.sensor_available and telem.sensor_available[key] then
            return `{math.round(telem[key])}{unit}`
        end
        return nil
    end

    local hotspot = reading("temp_junction_c", "°C")
    local edge = reading("temp_edge_c", "°C")
    local load = reading("gpu_busy_percent", "%")
    local power = reading("power_watts", "W")
    local temperature = hotspot or edge
    local metric = noctalia.getConfig("bar_metric")
    barWidget.setText((if metric == "gpu_busy" then load elseif metric == "edge" then edge else temperature) or "")

    if status.available and type(telem.gpu_name) == "string" and telem.gpu_name ~= "" then
        table.insert(tooltip, telem.gpu_name)
    end
    if temperature or load or power then
        local sensor = if hotspot then tr("widget.hotspot", "Hotspot") else tr("widget.edge", "Borda")
        table.insert(tooltip, tr("widget.sensors", "GPU: {load} | {sensor}: {temperature} | Consumo: {power}", {
            load = load or "—", sensor = sensor, temperature = temperature or "—", power = power or "—",
        }))
    end
    if available then
        table.insert(tooltip, tr("color.summary", "Saturação: {sat}% | Contraste: {con}% | Brilho: {bri}%", {
            sat = dev.saturation, con = dev.contrast, bri = dev.brightness,
        }))
    end
    local rule = (status.automation or {}).active_rule
    if rule then
        table.insert(tooltip, tr("widget.active_rule", "Regra Niri: {pattern}", { pattern = rule }))
    end
    barWidget.setTooltip(table.concat(tooltip, "\n"))
    barWidget.setVisible(available or not noctalia.getConfig("hide_inactive"))
end

function onRightClick()
    if calibrationAvailable(currentStatus) then
        noctalia.state.set("command", {
            action = "set", enabled = if currentStatus.device.enabled == 1 then 0 else 1, immediate = true,
        })
    end
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
