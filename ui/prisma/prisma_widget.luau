--!nonstrict
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

    local metricStr = ""
    local metricChoice = if type(barMetric) == "string" and barMetric ~= "" then barMetric else "hotspot"

    if metricChoice == "edge" then
        local tempEdge = if type(telem.temp_edge_c) == "number" then telem.temp_edge_c else 0
        metricStr = `{math.floor(tempEdge + 0.5)}°C`
    elseif metricChoice == "gpu_busy" then
        local gpuBusy = if type(telem.gpu_busy_percent) == "number" then telem.gpu_busy_percent else 0
        metricStr = `{gpuBusy}%`
    else
        local tempJunc = if type(telem.temp_junction_c) == "number" then telem.temp_junction_c else 0
        metricStr = `{math.floor(tempJunc + 0.5)}°C`
    end

    barWidget.setText(metricStr)

    local gpuBusy = if type(telem.gpu_busy_percent) == "number" then telem.gpu_busy_percent else 0
    local tempJunc = if type(telem.temp_junction_c) == "number" then math.floor(telem.temp_junction_c + 0.5) else 0
    local powerW = if type(telem.power_watts) == "number" then math.floor(telem.power_watts + 0.5) else 0
    local sat = if type(dev.saturation) == "number" then dev.saturation else 100
    local con = if type(dev.contrast) == "number" then dev.contrast else 100
    local bri = if type(dev.brightness) == "number" then dev.brightness else 0

    local tooltip = `Prisma Control Center\n`
        .. `GPU: {gpuBusy}% | Hotspot: {tempJunc}°C | Consumo: {powerW}W\n`
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
