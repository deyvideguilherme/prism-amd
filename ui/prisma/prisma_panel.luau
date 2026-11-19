--!nonstrict
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
