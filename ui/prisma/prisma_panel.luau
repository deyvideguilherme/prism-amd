--!nonstrict
-- Prisma Control Center - Interactive Desktop Panel
-- Visual control center featuring live hardware telemetry, DCN color calibration, and profile management.
-- Uses standard Noctalia native primitives (ui.box, ui.column, ui.row, ui.label, ui.button, ui.progress, ui.input, ui.scroll).

local currentTab = "performance" -- "performance" | "color" | "profiles"
local newProfileName = ""
local rulePattern = ""
local rulePatternRevision = 0
local ruleProfile = nil
local hasPendingColorUpdate = false
local lastMutationErrorSeq = 0

-- Cached local state to provide instant feedback before service roundtrips
local localDevice = {
    enabled = 1,
    saturation = 100,
    contrast = 100,
    brightness = 0,
}

local statusData = nil
local render

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

    local isApu = (telem.is_apu == true) or (telem.gpu_type == "APU")
    local powerW = if type(telem.power_watts) == "number" then math.floor(telem.power_watts + 0.5) else 0
    local powerMaxW = if type(telem.power_cap_watts) == "number" and telem.power_cap_watts > 0
        then math.max(1, math.floor(telem.power_cap_watts + 0.5))
        else (if isApu then 65 else 300)

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
            ui.label({ text = tr("panel.performance.load", "CARGA DA GPU"), fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.label({ text = `{sclkMhz} MHz`, fontSize = 12, color = "on_surface_variant" }),
        }),
        ui.row({ align = "baseline", gap = 6 }, {
            ui.label({ text = `{gpuBusy}%`, fontSize = 22, fontWeight = "bold", color = "primary" }),
            ui.label({ text = tr("panel.performance.activity", "Atividade de processamento gráfico"), fontSize = 11, color = "on_surface_variant" }),
        }),
        ui.progress({
            height = 8,
            radius = 4,
            progress = clamp(gpuBusy / 100, 0, 1),
            fill = "primary",
        }),
    })

    -- 2. Temperatures Block (Hotspot & Edge)
    local tempRightLabel = if hasHotspot then
        tr("panel.performance.edge", "Borda: {temperature}°C", { temperature = tostring(tempEdge) })
    else
        tr("panel.performance.edge_only", "Sensor Único (Borda)")
    local tempSubtitle = if hasHotspot then
        tr("panel.performance.hotspot_description", "Temperatura interna máxima da GPU (Hotspot)")
    elseif isApu then
        tr("panel.performance.apu_temperature", "Temperatura do encapsulamento da APU (Edge)")
    else
        tr("panel.performance.gpu_temperature", "Temperatura da GPU (Edge)")
    local tempFill = if mainTemp > tempWarn then "error" else "secondary"

    local tempBlock = ui.column({ gap = 4, align = "stretch" }, {
        ui.row({ align = "center", justify = "space_between" }, {
            ui.label({ text = tr("panel.performance.temperature", "TEMPERATURA"), fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
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
    local powerSubtitle = if isApu then
        tr("panel.performance.package_power", "Potência elétrica do pacote APU (PPT)")
    else
        tr("panel.performance.board_power", "Potência elétrica da placa")

    local powerBlock = ui.column({ gap = 4, align = "stretch" }, {
        ui.row({ align = "center", justify = "space_between" }, {
            ui.label({ text = tr("panel.performance.power", "CONSUMO DE ENERGIA"), fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
            ui.label({
                text = tr("panel.performance.power_cap", "Teto: {power} W", { power = tostring(powerMaxW) }),
                fontSize = 12, color = "on_surface_variant",
            }),
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
    local vramTitle = tr("panel.performance.vram", "MEMÓRIA VRAM")
    local vramRightLabel = `{string.format("%.1f", vramUsed / 1024)} / {string.format("%.1f", vramTotal / 1024)} GiB`
    local vramPercent = `{math.floor(vramRatio * 100 + 0.5)}%`
    local vramSubtitle = tr("panel.performance.allocated", "{memory} MiB alocados", { memory = tostring(math.floor(vramUsed)) })
    local vramProgressVal = vramRatio

    if isApu and gttTotal > 0 then
        vramTitle = tr("panel.performance.unified_memory", "MEMÓRIA GRÁFICA (VRAM + GTT)")
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
    local gpuName = if type(telem.gpu_name) == "string" and telem.gpu_name ~= "" then telem.gpu_name else nil
    local cardDisplay = if gpuName then `{card} ({if isApu then "APU" else "dGPU"}) — {gpuName}` else `{card} ({if isApu then "APU" else "dGPU"})`
    local infoRow = ui.column({ gap = 4, align = "stretch" }, {
        ui.row({ justify = "space_between", gap = 8, align = "center" }, {
            ui.label({ text = cardDisplay, fontSize = 12, fontWeight = "bold", color = "on_surface" }),
            ui.label({
                text = if statusData and statusData.available then
                    tr("panel.performance.active", "Hardware Ativo")
                else
                    tr("panel.performance.offline", "Offline"),
                fontSize = 12,
                fontWeight = "bold",
                color = if statusData and statusData.available then "primary" else "error",
            }),
        }),
        ui.row({ gap = 12, align = "center" }, {
            ui.label({ text = pciId, fontSize = 11, color = "on_surface_variant" }),
            ui.label({ text = devId, fontSize = 11, color = "on_surface_variant" }),
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

local presets = {
    standard = { saturation = 100, contrast = 100, brightness = 0 },
    competitive = { saturation = 160, contrast = 115, brightness = 10 },
    cinema = { saturation = 120, contrast = 105, brightness = -5 },
    mono = { saturation = 0, contrast = 110, brightness = 0 },
}

local function matchesColors(settings)
    return localDevice.saturation == settings.saturation
        and localDevice.contrast == settings.contrast
        and localDevice.brightness == settings.brightness
end

local function deviceAvailable()
    return if statusData and statusData.device and statusData.device.available ~= nil then
        statusData.device.available == true
    elseif statusData and statusData.device_available ~= nil then
        statusData.device_available == true
    else
        true
end

local function colorView()
    local isDeviceAvailable = deviceAvailable()
    local isEnabled = isDeviceAvailable and (localDevice.enabled == 1)

    -- 1. Master Enable Toggle Row
    local masterRow = ui.row({ align = "center", justify = "space_between" }, {
        ui.column({ gap = 2, flexGrow = 1 }, {
            ui.label({ text = tr("panel.color.title", "Calibragem de Cor em Hardware"), fontSize = 14, fontWeight = "bold" }),
            ui.label({
                text = if not isDeviceAvailable then
                    tr("panel.color.unavailable", "Módulo /dev/chroma_amd não encontrado no kernel. Calibragem indisponível.")
                else
                    tr("panel.color.description", "Ajusta saturação, contraste e brilho diretamente na saída de vídeo da placa"),
                fontSize = 11,
                color = if not isDeviceAvailable then "error" else "on_surface_variant",
            }),
        }),
        ui.button({
            text = if not isDeviceAvailable then
                tr("panel.color.offline", "Módulo Offline")
            else
                (if isEnabled then tr("panel.color.enabled", "Calibragem: ATIVADA") else tr("panel.color.disabled", "Calibragem: DESATIVADA")),
            glyph = if not isDeviceAvailable then "alert-triangle" else (if isEnabled then "check" else "x"),
            variant = if isDeviceAvailable and isEnabled then "primary" else "outline",
            onClick = if isDeviceAvailable then "onToggleMaster" else nil,
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
                    tooltip = tr("panel.color.restore", "Restaurar padrão ({value}%)", { value = tostring(defaultVal) }),
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
        tr("panel.color.saturation", "Saturação"),
        tr("panel.color.saturation_description", "Intensidade e vivacidade das cores na tela (0..400%)"),
        localDevice.saturation,
        0, 400, 100,
        function(val) localDevice.saturation = clamp(val, 0, 400) end,
        "onSaturationSub1",
        "onSaturationAdd1",
        "onSaturationReset"
    )

    local conBlock = parameterBlock(
        "contrast",
        tr("panel.color.contrast", "Contraste"),
        tr("panel.color.contrast_description", "Diferença entre tons claros e escuros (10..300%)"),
        localDevice.contrast,
        10, 300, 100,
        function(val) localDevice.contrast = clamp(val, 10, 300) end,
        "onContrastSub1",
        "onContrastAdd1",
        "onContrastReset"
    )

    local briBlock = parameterBlock(
        "brightness",
        tr("panel.color.brightness", "Brilho"),
        tr("panel.color.brightness_description", "Nível geral de luminosidade da imagem (-100..100%)"),
        localDevice.brightness,
        -100, 100, 0,
        function(val) localDevice.brightness = clamp(val, -100, 100) end,
        "onBrightnessSub1",
        "onBrightnessAdd1",
        "onBrightnessReset"
    )

    -- 3. Presets Rápidos
    local function presetButton(preset, text, glyph, onClick)
        return ui.button({
            text = text,
            glyph = glyph,
            variant = if isEnabled and matchesColors(preset) then "primary" else "outline",
            enabled = isEnabled,
            onClick = onClick,
            flexGrow = 1,
        })
    end
    local presetsBlock = ui.column({ gap = 8, align = "stretch" }, {
        ui.label({ text = tr("panel.color.presets", "PRESETS RÁPIDOS"), fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
        ui.row({ gap = 8, align = "center" }, {
            presetButton(presets.standard, tr("panel.color.standard", "Padrão"), "rotate", "onPresetStandard"),
            presetButton(presets.competitive, tr("panel.color.competitive", "Competitivo"), "device-gamepad-2", "onPresetCompetitive"),
            presetButton(presets.cinema, tr("panel.color.cinema", "Cinema"), "movie", "onPresetCinema"),
            presetButton(presets.mono, tr("panel.color.mono", "Mono"), "palette", "onPresetMono"),
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

local function automationView(automation, profiles)
    local enabled = automation.enabled == true
    local indicator = nil
    if automation.niri_available == false then
        indicator = tr("panel.automation.niri_unavailable", "Niri indisponível")
    elseif type(automation.active_rule) == "string" then
        indicator = tr("panel.automation.active", "Regra ativa: {pattern}", { pattern = automation.active_rule })
    end

    local options = {}
    local selectedIndex = 0
    for index, profile in ipairs(profiles) do
        options[index] = profile.name
        if profile.name == ruleProfile then
            selectedIndex = index - 1
        end
    end
    local selectedProfile = options[selectedIndex + 1]

    local lastApp = automation.last_app_id
    local patternControls = {
        ui.input({
            key = `rule_pattern_{rulePatternRevision}`,
            placeholder = tr("panel.automation.pattern", "app_id (ex.: cs2, steam_app_*)"),
            value = rulePattern,
            flexGrow = 1,
            onChange = "onRulePatternChange",
        }),
    }
    if type(lastApp) == "string" and lastApp ~= "" then
        table.insert(patternControls, ui.button({
            key = "rule_use_last_app",
            glyph = "focus-2",
            text = tr("panel.automation.use_last_app", "Usar última janela"),
            tooltip = lastApp,
            variant = "outline",
            onClick = function()
                rulePattern = lastApp
                rulePatternRevision += 1
                render()
            end,
        }))
    end

    local rules = automation.rules or {}
    local rows = {}
    for _, rule in ipairs(rules) do
        local pattern = rule.pattern
        table.insert(rows, ui.row({ key = `rule_{pattern}`, gap = 8, align = "center" }, {
            ui.label({ text = `{pattern} → {rule.profile}`, flexGrow = 1, maxWidth = 440, fontSize = 12 }),
            ui.button({
                key = `rule_delete_{pattern}`,
                glyph = "trash",
                variant = "destructive",
                controlSize = "sm",
                tooltip = tr("panel.automation.delete", "Excluir regra '{pattern}'", { pattern = pattern }),
                onClick = function() sendCommand({ action = "rule_delete", pattern = pattern }) end,
            }),
        }))
    end
    if #rows == 0 then
        table.insert(rows, ui.label({
            text = tr("panel.automation.empty", "Nenhuma regra configurada"),
            fontSize = 12,
            color = "on_surface_variant",
        }))
    end

    local heading = {
        ui.label({ text = tr("panel.automation.title", "Troca Automática (Niri)"), fontSize = 14, fontWeight = "bold" }),
    }
    if indicator then
        table.insert(heading, ui.label({
            text = indicator,
            maxWidth = 340,
            fontSize = 11,
            color = "on_surface_variant",
        }))
    end

    return ui.column({ gap = 8, align = "stretch" }, {
        ui.row({ gap = 8, align = "center", justify = "space_between" }, {
            ui.column({ gap = 2, flexGrow = 1 }, heading),
            ui.button({
                key = "rule_toggle",
                glyph = if enabled then "check" else "x",
                text = if enabled then tr("panel.automation.enabled", "Ativada") else tr("panel.automation.disabled", "Desativada"),
                variant = if enabled then "primary" else "outline",
                onClick = function() sendCommand({ action = "rule_toggle", enabled = not enabled }) end,
            }),
        }),
        ui.row({ gap = 8, align = "center" }, patternControls),
        ui.row({ gap = 8, align = "center" }, {
            ui.select({
                key = "rule_profile",
                options = options,
                selectedIndex = selectedIndex,
                placeholder = tr("panel.automation.profile", "Selecionar perfil salvo"),
                enabled = #options > 0,
                flexGrow = 1,
                onChange = function(index)
                    local selected = tonumber(index)
                    ruleProfile = if selected then options[selected + 1] else nil
                    render()
                end,
            }),
            ui.button({
                key = "rule_add",
                glyph = "plus",
                text = tr("panel.automation.add", "Adicionar"),
                variant = "primary",
                enabled = rulePattern ~= "" and selectedProfile ~= nil,
                onClick = function()
                    if rulePattern ~= "" and selectedProfile then
                        sendCommand({ action = "rule_set", pattern = rulePattern, profile = selectedProfile })
                    end
                end,
            }),
        }),
        ui.column({ gap = 6, align = "stretch" }, rows),
    })
end

local function profilesView()
    local profiles = if statusData and statusData.profiles then statusData.profiles else {}
    local automation = if statusData and statusData.automation then statusData.automation else {}

    -- Save Profile Block
    local saveBlock = ui.column({ gap = 8, align = "stretch" }, {
        ui.label({ text = tr("panel.profiles.save_current", "SALVAR ESTADO ATUAL"), fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
        ui.row({ gap = 8, align = "center" }, {
            ui.input({
                key = "profile_input",
                placeholder = tr("panel.profiles.placeholder", "Nome do perfil (ex: jogos, filmes)"),
                value = newProfileName,
                flexGrow = 1,
                onChange = "onProfileNameChange",
                onSubmit = "onSaveProfileSubmit",
            }),
            ui.button({
                text = tr("panel.profiles.save", "Salvar"),
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
            text = tr("panel.profiles.empty", "Nenhum perfil salvo ainda"),
            color = "on_surface_variant",
            fontSize = 12,
        }))
    else
        for _, p in ipairs(profiles) do
            local name = p.name
            local sat = p.saturation or 100
            local con = p.contrast or 100
            local bri = p.brightness or 0
            local isActive = deviceAvailable() and matchesColors(p)
                and (p.enabled == nil or localDevice.enabled == p.enabled)
            local isStartupDefault = name == automation.default_profile

            local onApply = function()
                hasPendingColorUpdate = false
                sendCommand({ action = "profile_apply", name = name })
            end

            local onDelete = function()
                sendCommand({ action = "profile_delete", name = name })
            end

            local details = {
                ui.label({ text = name, fontSize = 14, fontWeight = "bold" }),
                ui.label({
                    text = tr("color.summary", "Saturação: {sat}% | Contraste: {con}% | Brilho: {bri}%", {
                        sat = tostring(sat), con = tostring(con), bri = tostring(bri),
                    }),
                    fontSize = 11,
                    color = "on_surface_variant",
                }),
            }
            if isStartupDefault then
                table.insert(details, ui.label({
                    text = tr("panel.profiles.startup_default", "Padrão de Inicialização"),
                    fontSize = 11,
                    fontWeight = "bold",
                    color = "primary",
                }))
            end

            table.insert(profileRows, ui.row({ key = `profile_{name}`, gap = 6, align = "center", justify = "space_between" }, {
                ui.column({ gap = 2, flexGrow = 1 }, details),
                ui.row({ gap = 6, align = "center" }, {
                    ui.button({
                        text = if isActive then tr("panel.profiles.active", "Ativo") else tr("panel.profiles.apply", "Aplicar"),
                        glyph = "check",
                        variant = if isActive then "outline" else "primary",
                        enabled = not isActive,
                        onClick = onApply,
                    }),
                    ui.button({
                        key = `profile_default_{name}`,
                        glyph = "star",
                        variant = if isStartupDefault then "primary" else "outline",
                        tooltip = if isStartupDefault then
                            tr("panel.profiles.clear_startup_default", "Remover '{name}' como padrão de inicialização", { name = name })
                        else
                            tr("panel.profiles.set_startup_default", "Definir '{name}' como padrão de inicialização", { name = name }),
                        onClick = function()
                            if isStartupDefault then
                                sendCommand({ action = "rule_clear_default" })
                            else
                                sendCommand({ action = "rule_set_default", profile = name })
                            end
                        end,
                    }),
                    ui.button({
                        glyph = "trash",
                        variant = "destructive",
                        tooltip = tr("panel.profiles.delete", "Excluir perfil '{name}'", { name = name }),
                        onClick = onDelete,
                    }),
                }),
            }))
        end
    end

    local listBlock = ui.column({ gap = 8, align = "stretch" }, {
        ui.label({ text = tr("panel.profiles.saved", "PERFIS SALVOS"), fontSize = 11, fontWeight = "bold", color = "on_surface_variant" }),
        ui.column({ gap = 10, align = "stretch" }, profileRows),
    })

    -- Profile Actions Footer
    local actionsRow = ui.row({ align = "center", justify = "space_between" }, {
        ui.button({
            glyph = "refresh",
            text = tr("panel.profiles.reload", "Recarregar Lista"),
            variant = "ghost",
            onClick = "onRefreshProfilesClicked",
        }),
        ui.button({
            text = tr("panel.profiles.restore", "Restaurar Padrões"),
            glyph = "rotate",
            variant = "destructive",
            onClick = "onResetDefaultsClicked",
        }),
    })

    return ui.column({ gap = 16, align = "stretch" }, {
        saveBlock,
        listBlock,
        actionsRow,
        automationView(automation, profiles),
    })
end

-- ── Main Panel Render ────────────────────────────────────────────────────────

render = function()
    -- Check if a mutation error occurred to unlock optimistic state and resync
    if statusData and type(statusData.mutation_error_seq) == "number" then
        if statusData.mutation_error_seq > lastMutationErrorSeq then
            lastMutationErrorSeq = statusData.mutation_error_seq
            hasPendingColorUpdate = false
        end
    end

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
            ui.label({ text = tr("name", "Prisma Control Center"), fontSize = 16, fontWeight = "bold", color = "on_surface" }),
        }),
        ui.button({ glyph = "x", variant = "ghost", tooltip = tr("panel.close", "Fechar"), onClick = "onCloseClicked" }),
    })

    -- Navigation tab buttons
    local nav = ui.row({ gap = 8, align = "center" }, {
        ui.button({
            text = tr("panel.tabs.performance", "Desempenho"),
            glyph = "chart-line",
            variant = if currentTab == "performance" then "primary" else "ghost",
            onClick = "onTabPerformance",
            flexGrow = 1,
        }),
        ui.button({
            text = tr("panel.tabs.color", "Tela & Cor"),
            glyph = "adjustments",
            variant = if currentTab == "color" then "primary" else "ghost",
            onClick = "onTabColor",
            flexGrow = 1,
        }),
        ui.button({
            text = tr("panel.tabs.profiles", "Perfis"),
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

local function notifyPanelOpen()
    sendCommand({ action = "panel_state", open = true, tab = currentTab })
    if currentTab == "profiles" then
        sendCommand({ action = "profile_list" })
    end
end

local function showTab(tab)
    currentTab = tab
    notifyPanelOpen()
    render()
end

function onTabPerformance()
    showTab("performance")
end

function onTabColor()
    showTab("color")
end

function onTabProfiles()
    showTab("profiles")
end

function onOpen()
    notifyPanelOpen()
end

function onClose()
    sendCommand({ action = "panel_state", open = false })
end

function onCloseClicked()
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

local function applyPreset(preset)
    localDevice.saturation = preset.saturation
    localDevice.contrast = preset.contrast
    localDevice.brightness = preset.brightness
    localDevice.enabled = 1
    sendColorUpdate(true)
    render()
end

function onPresetStandard()
    applyPreset(presets.standard)
end

function onPresetCompetitive()
    applyPreset(presets.competitive)
end

function onPresetCinema()
    applyPreset(presets.cinema)
end

function onPresetMono()
    applyPreset(presets.mono)
end

function onProfileNameChange(text)
    local wasEmpty = (newProfileName == "")
    newProfileName = noctalia.string.trim(text)
    local isEmpty = (newProfileName == "")
    if wasEmpty ~= isEmpty then
        render()
    end
end

function onRulePatternChange(text)
    local wasEmpty = rulePattern == ""
    rulePattern = text
    if wasEmpty ~= (rulePattern == "") then
        render()
    end
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

-- Initial render
statusData = noctalia.state.get("status")
if statusData and type(statusData.mutation_error_seq) == "number" then
    lastMutationErrorSeq = statusData.mutation_error_seq
end
render()
