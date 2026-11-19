--!nonstrict
-- Prisma Control Center - Interactive Desktop Panel
-- Visual control center featuring live hardware telemetry, DCN color calibration, and profile management.

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
    local hist = (if statusData and statusData.history then statusData.history else {})

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

    local histGpu = if type(hist.gpu_busy) == "table" and #hist.gpu_busy > 0 then hist.gpu_busy else { 0 }
    local histTemp = if type(hist.temp_junction) == "table" and #hist.temp_junction > 0 then hist.temp_junction else { 0 }
    local histPower = if type(hist.power) == "table" and #hist.power > 0 then hist.power else { 0 }

    -- Card 1: GPU Load
    local gpuCard = ui.box({
        fill = "surface_container",
        radius = 12,
        paddingH = 14,
        paddingV = 12,
        flexGrow = 1,
    }, {
        ui.column({ gap = 8, align = "stretch" }, {
            ui.row({ align = "center", justify = "space_between" }, {
                ui.label({ text = "CARGA DA GPU", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
                ui.label({ text = `{sclkMhz} MHz`, fontSize = 12, color = "on_surface_variant" }),
            }),
            ui.row({ align = "baseline", gap = 4 }, {
                ui.label({ text = `{gpuBusy}%`, fontSize = 26, fontWeight = "bold", color = "primary" }),
            }),
            ui.graph({
                values = histGpu,
                height = 42,
                color = "primary",
                fillOpacity = 0.25,
                lineWidth = 2,
            }),
        }),
    })

    -- Card 2: Temperature
    local tempCard = ui.box({
        fill = "surface_container",
        radius = 12,
        paddingH = 14,
        paddingV = 12,
        flexGrow = 1,
    }, {
        ui.column({ gap = 8, align = "stretch" }, {
            ui.row({ align = "center", justify = "space_between" }, {
                ui.label({ text = "TEMPERATURA", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
                ui.label({ text = `Edge: {tempEdge}°C`, fontSize = 12, color = "on_surface_variant" }),
            }),
            ui.row({ align = "baseline", gap = 4 }, {
                ui.label({
                    text = `{tempJunc}°C`,
                    fontSize = 26,
                    fontWeight = "bold",
                    color = if tempJunc > 85 then "error" else "secondary",
                }),
                ui.label({ text = "Hotspot", fontSize = 12, color = "on_surface_variant" }),
            }),
            ui.graph({
                values = histTemp,
                height = 42,
                color = if tempJunc > 85 then "error" else "secondary",
                fillOpacity = 0.25,
                lineWidth = 2,
            }),
        }),
    })

    -- Card 3: Power Consumption
    local powerCard = ui.box({
        fill = "surface_container",
        radius = 12,
        paddingH = 14,
        paddingV = 12,
        flexGrow = 1,
    }, {
        ui.column({ gap = 8, align = "stretch" }, {
            ui.label({ text = "CONSUMO DE ENERGIA", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.row({ align = "baseline", gap = 4 }, {
                ui.label({ text = `{powerW} W`, fontSize = 26, fontWeight = "bold", color = "on_surface" }),
            }),
            ui.graph({
                values = histPower,
                height = 42,
                color = "outline_variant",
                fillOpacity = 0.25,
                lineWidth = 2,
            }),
        }),
    })

    -- Card 4: VRAM Allocation
    local vramCard = ui.box({
        fill = "surface_container",
        radius = 12,
        paddingH = 14,
        paddingV = 12,
        flexGrow = 1,
    }, {
        ui.column({ gap = 8, align = "stretch" }, {
            ui.row({ align = "center", justify = "space_between" }, {
                ui.label({ text = "MEMÓRIA VRAM", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
                ui.label({
                    text = `{string.format("%.1f", vramUsed / 1024)} / {string.format("%.1f", vramTotal / 1024)} GiB`,
                    fontSize = 12,
                    color = "on_surface_variant",
                }),
            }),
            ui.row({ align = "baseline", gap = 4 }, {
                ui.label({ text = `{math.floor(vramRatio * 100 + 0.5)}%`, fontSize = 26, fontWeight = "bold", color = "primary" }),
            }),
            ui.progress({
                value = vramRatio,
                color = "primary",
            }),
        }),
    })

    -- Hardware Identity Footer Box
    local infoBox = ui.box({
        fill = "surface_container",
        radius = 12,
        paddingH = 14,
        paddingV = 10,
    }, {
        ui.row({ align = "center", justify = "space_between" }, {
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
        }),
    })

    return ui.column({ gap = 12, align = "stretch" }, {
        ui.row({ gap = 12, align = "stretch" }, { gpuCard, tempCard }),
        ui.row({ gap = 12, align = "stretch" }, { powerCard, vramCard }),
        infoBox,
    })
end

-- ── Tab 2: Tela & Cor (DCN Color Adjustments) ────────────────────────────────

local function colorView()
    local isEnabled = (localDevice.enabled == 1)

    -- Master Enable Toggle Box
    local masterBox = ui.box({
        fill = "surface_container",
        radius = 12,
        paddingH = 16,
        paddingV = 12,
    }, {
        ui.row({ align = "center", justify = "space_between" }, {
            ui.column({ gap = 2 }, {
                ui.label({ text = "Injeção de Hardware CTM & GAMMA_LUT", fontSize = 14, fontWeight = "bold" }),
                ui.label({
                    text = "Aplica calibragem de saturação, contraste e brilho direto no display controller",
                    fontSize = 11,
                    color = "on_surface_variant",
                }),
            }),
            ui.toggle({
                checked = isEnabled,
                onChange = "onToggleMaster",
            }),
        }),
    })

    -- Slider Row Helper
    local function sliderCard(title, subtitle, value, minVal, maxVal, onChange, onDragEnd, onDec, onInc, onReset, defaultVal)
        return ui.box({
            fill = "surface_container",
            radius = 12,
            paddingH = 16,
            paddingV = 12,
        }, {
            ui.column({ gap = 8, align = "stretch" }, {
                ui.row({ align = "center", justify = "space_between" }, {
                    ui.column({ gap = 2 }, {
                        ui.label({ text = title, fontSize = 13, fontWeight = "bold" }),
                        ui.label({ text = subtitle, fontSize = 11, color = "on_surface_variant" }),
                    }),
                    ui.row({ gap = 6, align = "center" }, {
                        ui.label({ text = `{value}%`, fontSize = 14, fontWeight = "bold", color = "primary" }),
                        ui.button({
                            text = "-5",
                            variant = "ghost",
                            enabled = isEnabled and value > minVal,
                            onClick = onDec,
                        }),
                        ui.button({
                            text = "+5",
                            variant = "ghost",
                            enabled = isEnabled and value < maxVal,
                            onClick = onInc,
                        }),
                        ui.button({
                            glyph = "rotate",
                            variant = "ghost",
                            tooltip = `Restaurar padrão ({defaultVal}%)`,
                            enabled = isEnabled and value ~= defaultVal,
                            onClick = onReset,
                        }),
                    }),
                }),
                ui.slider({
                    min = minVal,
                    max = maxVal,
                    step = 1,
                    value = value,
                    enabled = isEnabled,
                    onChange = onChange,
                    onDragEnd = onDragEnd,
                    flexGrow = 1,
                }),
            }),
        })
    end

    local satCard = sliderCard(
        "Saturação",
        "Digital Vibrance em hardware Rec.709",
        localDevice.saturation,
        0, 400,
        "onSaturationChange",
        "onSaturationDragEnd",
        "onSaturationDec",
        "onSaturationInc",
        "onSaturationReset",
        100
    )

    local conCard = sliderCard(
        "Contraste",
        "Fator de contraste linear da matriz CTM",
        localDevice.contrast,
        10, 300,
        "onContrastChange",
        "onContrastDragEnd",
        "onContrastDec",
        "onContrastInc",
        "onContrastReset",
        100
    )

    local briCard = sliderCard(
        "Brilho",
        "Offset na curva GAMMA_LUT do hardware",
        localDevice.brightness,
        -100, 100,
        "onBrightnessChange",
        "onBrightnessDragEnd",
        "onBrightnessDec",
        "onBrightnessInc",
        "onBrightnessReset",
        0
    )

    -- Presets Rápidos
    local presetsBox = ui.box({
        fill = "surface_container",
        radius = 12,
        paddingH = 16,
        paddingV = 12,
    }, {
        ui.column({ gap = 8, align = "stretch" }, {
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
        }),
    })

    return ui.column({ gap = 10, align = "stretch" }, {
        masterBox,
        satCard,
        conCard,
        briCard,
        presetsBox,
    })
end

-- ── Tab 3: Perfis (Profiles Management) ──────────────────────────────────────

local function profilesView()
    local profiles = if statusData and statusData.profiles then statusData.profiles else {}

    -- Save Profile Box
    local saveBox = ui.box({
        fill = "surface_container",
        radius = 12,
        paddingH = 16,
        paddingV = 12,
    }, {
        ui.column({ gap = 8, align = "stretch" }, {
            ui.label({ text = "SALVAR ESTADO ATUAL", fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.row({ gap = 8, align = "center" }, {
                ui.input({
                    key = "profile_input",
                    placeholder = "Nome do perfil (ex: gaming, movie)",
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
        }),
    })

    -- Profile List Items
    local profileRows = {}
    if #profiles == 0 then
        table.insert(profileRows, ui.box({
            fill = "surface_container",
            radius = 12,
            paddingH = 16,
            paddingV = 16,
        }, {
            ui.label({
                text = "Nenhum perfil salvo encontrado em ~/.config/prisma/profiles/",
                color = "on_surface_variant",
                fontSize = 12,
            }),
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

            table.insert(profileRows, ui.box({
                key = `profile_{name}`,
                fill = "surface_container",
                radius = 12,
                paddingH = 16,
                paddingV = 10,
            }, {
                ui.row({ align = "center", justify = "space_between" }, {
                    ui.column({ gap = 2 }, {
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
                }),
            }))
        end
    end

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

    return ui.column({ gap = 12, align = "stretch" }, {
        saveBox,
        ui.column({ gap = 8, align = "stretch" }, profileRows),
        actionsRow,
    })
end

-- ── Main Panel Render ────────────────────────────────────────────────────────

render = function()
    -- Sync local state with status if not actively dragging
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
        banner = ui.box({
            fill = "surface_container",
            border = "error",
            borderWidth = 1,
            radius = 10,
            paddingH = 12,
            paddingV = 8,
        }, {
            ui.label({ text = statusData.error, color = "error", fontSize = 11 }),
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

function onToggleMaster(checked)
    localDevice.enabled = if checked then 1 else 0
    sendColorUpdate(true)
    render()
end

function onSaturationChange(val)
    local num = tonumber(val)
    if num then
        localDevice.saturation = clamp(math.floor(num + 0.5), 0, 400)
        sendColorUpdate(false)
        render()
    end
end

function onSaturationDragEnd(_val)
    sendColorUpdate(true)
end

function onSaturationDec()
    localDevice.saturation = clamp(localDevice.saturation - 5, 0, 400)
    sendColorUpdate(true)
    render()
end

function onSaturationInc()
    localDevice.saturation = clamp(localDevice.saturation + 5, 0, 400)
    sendColorUpdate(true)
    render()
end

function onSaturationReset()
    localDevice.saturation = 100
    sendColorUpdate(true)
    render()
end

function onContrastChange(val)
    local num = tonumber(val)
    if num then
        localDevice.contrast = clamp(math.floor(num + 0.5), 10, 300)
        sendColorUpdate(false)
        render()
    end
end

function onContrastDragEnd(_val)
    sendColorUpdate(true)
end

function onContrastDec()
    localDevice.contrast = clamp(localDevice.contrast - 5, 10, 300)
    sendColorUpdate(true)
    render()
end

function onContrastInc()
    localDevice.contrast = clamp(localDevice.contrast + 5, 10, 300)
    sendColorUpdate(true)
    render()
end

function onContrastReset()
    localDevice.contrast = 100
    sendColorUpdate(true)
    render()
end

function onBrightnessChange(val)
    local num = tonumber(val)
    if num then
        localDevice.brightness = clamp(math.floor(num + 0.5), -100, 100)
        sendColorUpdate(false)
        render()
    end
end

function onBrightnessDragEnd(_val)
    sendColorUpdate(true)
end

function onBrightnessDec()
    localDevice.brightness = clamp(localDevice.brightness - 5, -100, 100)
    sendColorUpdate(true)
    render()
end

function onBrightnessInc()
    localDevice.brightness = clamp(localDevice.brightness + 5, -100, 100)
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
