--!nonstrict
-- Prisma Control Center - Interactive Desktop Panel
-- Visual control center featuring live hardware telemetry, DCN color calibration, and profile management.
-- Uses standard Noctalia native primitives (ui.column, ui.row, ui.label, ui.button, ui.progress, ui.input, ui.scroll).

local currentTab = "performance" -- "performance" | "color" | "profiles"
local newProfileName = ""

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
    local tempJunc = if type(telem.temp_junction_c) == "number" then math.floor(telem.temp_junction_c + 0.5) else 0
    local tempEdge = if type(telem.temp_edge_c) == "number" then math.floor(telem.temp_edge_c + 0.5) else 0
    local powerW = if type(telem.power_watts) == "number" then math.floor(telem.power_watts + 0.5) else 0

    local vramUsed = if type(telem.vram_used_mib) == "number" then telem.vram_used_mib else 0
    local vramTotal = if type(telem.vram_total_mib) == "number" and telem.vram_total_mib > 0 then telem.vram_total_mib else 1
    local vramRatio = clamp(vramUsed / vramTotal, 0, 1)

    local card = if type(telem.card) == "string" and telem.card ~= "" then telem.card else "card1"
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
    local tempBlock = ui.column({ gap = 4, align = "stretch" }, {
        ui.row({ align = "center", justify = "space_between" }, {
            ui.label({ text = "TEMPERATURA", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.label({ text = `Edge: {tempEdge}°C`, fontSize = 12, color = "on_surface_variant" }),
        }),
        ui.row({ align = "baseline", gap = 6 }, {
            ui.label({
                text = `{tempJunc}°C`,
                fontSize = 22,
                fontWeight = "bold",
                color = if tempJunc > 85 then "error" else "secondary",
            }),
            ui.label({ text = "Sensor Hotspot / Junction", fontSize = 11, color = "on_surface_variant" }),
        }),
        ui.progress({
            height = 8,
            radius = 4,
            progress = clamp(tempJunc / 110, 0, 1),
            fill = if tempJunc > 85 then "error" else "secondary",
        }),
    })

    -- 3. Power Consumption Block
    local powerBlock = ui.column({ gap = 4, align = "stretch" }, {
        ui.row({ align = "center", justify = "space_between" }, {
            ui.label({ text = "CONSUMO DE ENERGIA", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.label({ text = "hwmon", fontSize = 12, color = "on_surface_variant" }),
        }),
        ui.row({ align = "baseline", gap = 6 }, {
            ui.label({ text = `{powerW} W`, fontSize = 22, fontWeight = "bold", color = "on_surface" }),
            ui.label({ text = "Potência elétrica da placa", fontSize = 11, color = "on_surface_variant" }),
        }),
        ui.progress({
            height = 8,
            radius = 4,
            progress = clamp(powerW / 300, 0, 1),
            fill = "primary",
        }),
    })

    -- 4. VRAM Allocation Block
    local vramBlock = ui.column({ gap = 4, align = "stretch" }, {
        ui.row({ align = "center", justify = "space_between" }, {
            ui.label({ text = "MEMÓRIA VRAM", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.label({
                text = `{string.format("%.1f", vramUsed / 1024)} / {string.format("%.1f", vramTotal / 1024)} GiB`,
                fontSize = 12,
                color = "on_surface_variant",
            }),
        }),
        ui.row({ align = "baseline", gap = 6 }, {
            ui.label({ text = `{math.floor(vramRatio * 100 + 0.5)}%`, fontSize = 22, fontWeight = "bold", color = "primary" }),
            ui.label({ text = `{math.floor(vramUsed)} MiB alocados`, fontSize = 11, color = "on_surface_variant" }),
        }),
        ui.progress({
            height = 8,
            radius = 4,
            progress = vramRatio,
            fill = "primary",
        }),
    })

    -- Hardware Identity Footer Row
    local infoRow = ui.row({ align = "center", justify = "space_between" }, {
        ui.row({ gap = 12, align = "center" }, {
            ui.label({ text = card, fontSize = 12, fontWeight = "bold", color = "on_surface" }),
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

local function colorView()
    local isEnabled = (localDevice.enabled == 1)

    -- 1. Master Enable Toggle Row
    local masterRow = ui.row({ align = "center", justify = "space_between" }, {
        ui.column({ gap = 2, flexGrow = 1 }, {
            ui.label({ text = "Injeção de Hardware CTM & GAMMA_LUT", fontSize = 14, fontWeight = "bold" }),
            ui.label({
                text = "Aplica calibragem de saturação, contraste e brilho direto no display controller",
                fontSize = 11,
                color = "on_surface_variant",
            }),
        }),
        ui.button({
            text = if isEnabled then "Injeção: ATIVADA" else "Injeção: DESATIVADA",
            glyph = if isEnabled then "check" else "x",
            variant = if isEnabled then "primary" else "outline",
            onClick = "onToggleMaster",
        }),
    })

    -- 2. Precision Parameter Block Helper
    local function parameterBlock(title, subtitle, value, minVal, maxVal, progressVal, onSub10, onSub1, onAdd1, onAdd10, onReset, defaultVal)
        return ui.column({ gap = 6, align = "stretch" }, {
            ui.row({ align = "center", justify = "space_between" }, {
                ui.column({ gap = 2 }, {
                    ui.label({ text = title, fontSize = 13, fontWeight = "bold" }),
                    ui.label({ text = subtitle, fontSize = 11, color = "on_surface_variant" }),
                }),
                ui.label({ text = `{value}%`, fontSize = 16, fontWeight = "bold", color = "primary" }),
            }),
            ui.progress({
                height = 8,
                radius = 4,
                progress = progressVal,
                fill = if isEnabled then "primary" else "on_surface_variant",
            }),
            ui.row({ gap = 6, align = "center" }, {
                ui.button({
                    text = "-10",
                    variant = "ghost",
                    enabled = isEnabled and value > minVal,
                    onClick = onSub10,
                    flexGrow = 1,
                }),
                ui.button({
                    text = "-1",
                    variant = "ghost",
                    enabled = isEnabled and value > minVal,
                    onClick = onSub1,
                    flexGrow = 1,
                }),
                ui.button({
                    text = "+1",
                    variant = "ghost",
                    enabled = isEnabled and value < maxVal,
                    onClick = onAdd1,
                    flexGrow = 1,
                }),
                ui.button({
                    text = "+10",
                    variant = "ghost",
                    enabled = isEnabled and value < maxVal,
                    onClick = onAdd10,
                    flexGrow = 1,
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
    end

    local satProgress = clamp(localDevice.saturation / 400, 0, 1)
    local satBlock = parameterBlock(
        "Saturação",
        "Digital Vibrance em hardware Rec.709 (0..400%)",
        localDevice.saturation,
        0, 400,
        satProgress,
        "onSaturationSub10",
        "onSaturationSub1",
        "onSaturationAdd1",
        "onSaturationAdd10",
        "onSaturationReset",
        100
    )

    local conProgress = clamp((localDevice.contrast - 10) / (300 - 10), 0, 1)
    local conBlock = parameterBlock(
        "Contraste",
        "Fator de contraste linear da matriz CTM (10..300%)",
        localDevice.contrast,
        10, 300,
        conProgress,
        "onContrastSub10",
        "onContrastSub1",
        "onContrastAdd1",
        "onContrastAdd10",
        "onContrastReset",
        100
    )

    local briProgress = clamp((localDevice.brightness + 100) / 200, 0, 1)
    local briBlock = parameterBlock(
        "Brilho",
        "Offset na curva GAMMA_LUT do hardware (-100..100%)",
        localDevice.brightness,
        -100, 100,
        briProgress,
        "onBrightnessSub10",
        "onBrightnessSub1",
        "onBrightnessAdd1",
        "onBrightnessAdd10",
        "onBrightnessReset",
        0
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
                placeholder = "Nome do perfil (ex: gaming, cinema)",
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
            text = "Nenhum perfil salvo encontrado em ~/.config/prisma/profiles/",
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
            text = "Redefinir Padrões de Fábrica",
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
        localDevice.enabled = if type(dev.enabled) == "number" then dev.enabled else 1
        localDevice.saturation = if type(dev.saturation) == "number" then dev.saturation else 100
        localDevice.contrast = if type(dev.contrast) == "number" then dev.contrast else 100
        localDevice.brightness = if type(dev.brightness) == "number" then dev.brightness else 0
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

function onSaturationSub10()
    localDevice.saturation = clamp(localDevice.saturation - 10, 0, 400)
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

function onSaturationAdd10()
    localDevice.saturation = clamp(localDevice.saturation + 10, 0, 400)
    sendColorUpdate(true)
    render()
end

function onSaturationReset()
    localDevice.saturation = 100
    sendColorUpdate(true)
    render()
end

function onContrastSub10()
    localDevice.contrast = clamp(localDevice.contrast - 10, 10, 300)
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

function onContrastAdd10()
    localDevice.contrast = clamp(localDevice.contrast + 10, 10, 300)
    sendColorUpdate(true)
    render()
end

function onContrastReset()
    localDevice.contrast = 100
    sendColorUpdate(true)
    render()
end

function onBrightnessSub10()
    localDevice.brightness = clamp(localDevice.brightness - 10, -100, 100)
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

function onBrightnessAdd10()
    localDevice.brightness = clamp(localDevice.brightness + 10, -100, 100)
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
