--!nonstrict
-- Prisma Control Center - Bar Widget Indicator
-- Displays real-time hardware status and selected telemetry metric on the Noctalia bar.
-- Clicking toggles the attached interactive Control Center panel.

local barMetric = noctalia.getConfig("bar_metric")
local hideInactive = noctalia.getConfig("hide_inactive")

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

local function updateDisplay()
    barMetric = noctalia.getConfig("bar_metric")
    hideInactive = noctalia.getConfig("hide_inactive")

    local status = currentStatus
    if type(status) ~= "table" or not status.available then
        barWidget.setGlyph("palette")
        barWidget.setGlyphColor("on_surface_variant")
        barWidget.setText("")
        barWidget.setTooltip(tr("name", "Prisma Control Center") .. "\n"
            .. tr("widget.offline", "Hardware não disponível ou módulo não carregado"))
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

    local sensor = if hasHotspot then tr("widget.hotspot", "Hotspot") else tr("widget.edge", "Borda")
    local sensors = tr("widget.sensors", "GPU: {load}% | {sensor}: {temperature}°C | Consumo: {power}W", {
        load = gpuBusy, sensor = sensor, temperature = primaryTempRounded, power = powerW,
    })
    local colors = tr("color.summary", "Saturação: {sat}% | Contraste: {con}% | Brilho: {bri}%", {
        sat = sat, con = con, bri = bri,
    })
    local tooltip = tr("name", "Prisma Control Center") .. "\n" .. sensors .. "\n" .. colors

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
