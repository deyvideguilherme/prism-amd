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

-- check_ui.py supplies the current plugin sources and both translation catalogs.
local loaded, sources = pcall(require, "./ui_sources")
assert(loaded, "Run python3 tests/check_ui.py to load the plugin and its translations")

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
local catalogs = {
    ["pt-BR"] = decodeJson(sources.translations["pt-BR"]),
    en = decodeJson(sources.translations.en),
}
local language = "pt-BR"
local function translate(key, substitutions)
    local value = catalogs[language]
    for part in string.gmatch(key, "[^.]+") do
        if type(value) ~= "table" then return key end
        value = value[part]
    end
    if type(value) ~= "string" then return key end
    return (string.gsub(value, "{([%w_]+)}", function(name)
        if substitutions and substitutions[name] ~= nil then
            return tostring(substitutions[name])
        end
        return "{" .. name .. "}"
    end))
end

local mockConfig = {
    bar_metric = "hotspot",
    hide_inactive = false,
}

local mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","gpu_name":"AMD Radeon Graphics (Cezanne)","pci_id":"0000:07:00.0","device_id":"1002:1638","is_apu":true,"gpu_busy_percent":2,"sclk_mhz":400,"vram_used_mib":455.3,"vram_total_mib":512.0,"gtt_used_mib":643.9,"gtt_total_mib":7649.9,"temp_edge_c":34.0,"temp_junction_c":null,"power_watts":25.0}}]],
    stderr = "",
}

local mockProfilesResponse = { exitCode = 0, stdout = "[]", stderr = "" }

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
    tr = translate,
    json = {
        decode = decodeJson,
    },
    runAsync = function(cmd, cb)
        if string.find(cmd, "prisma-ctl status --json", 1, true) then
            cb(mockStatusResponse)
        elseif cmd == "prisma-ctl profile list --json" then
            cb(mockProfilesResponse)
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
    select = function(props) return { type = "select", props = props or {}, children = {} } end,
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

local function loadComponent(component, environment)
    local fn, message = loadstring(sources[component])
    assert(fn ~= nil, message)
    setfenv(fn, environment)
    fn()
end

loadComponent("service", mockEnv)
loadComponent("widget", mockEnv)
loadComponent("panel", mockEnv)
mockEnv.onOpen()

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

local footers = findNodes(tree1, function(n)
    if n.type ~= "column" or #n.children ~= 2 then return false end
    local identity, ids = n.children[1], n.children[2]
    return identity.type == "row" and ids.type == "row"
        and hasLabelWithText(identity, "card1 (APU) — AMD Radeon Graphics (Cezanne)")
        and hasLabelWithText(identity, "Hardware Ativo")
        and hasLabelWithText(ids, "0000:07:00.0")
        and hasLabelWithText(ids, "1002:1638")
end)
assert(#footers == 1, "GPU name/status and PCI IDs must occupy separate footer rows")
local footer = footers[1]
assert(footer.props.gap == 4 and footer.props.align == "stretch")
assert(footer.children[1].props.justify == "space_between" and footer.children[1].props.gap == 8)
assert(footer.children[2].props.gap == 12)
for _, label in ipairs(footer.children[2].children) do
    assert(label.props.fontSize == 11 and label.props.color == "on_surface_variant")
end

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

-- Cenário 5 — Telemetria desacoplada: GPU ativa e módulo /dev/chroma_amd ausente
print("[test_ui] Running Scenario 5: Decoupled telemetry with offline module...")
mockConfig.hide_inactive = false
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"available":false,"enabled":0,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","gpu_name":"AMD Radeon RX 9060 XT","gpu_type":"dGPU","pci_id":"0000:2d:00.0","device_id":"1002:7590","is_apu":false,"gpu_busy_percent":35,"sclk_mhz":2100,"vram_used_mib":4096.0,"vram_total_mib":16384.0,"gtt_used_mib":null,"gtt_total_mib":null,"temp_edge_c":48.0,"temp_junction_c":62.0,"power_watts":120.0}}]],
    stderr = "",
}
for _ = 1, 20 do
    mockEnv.update()
end
mockEnv.onConfigChanged()

-- Verificar que telemetria continua ativa
local st5 = mockState["status"]
assert(st5 ~= nil, "Scenario 5: status must be published")
assert(st5.available == true, "Scenario 5: status.available must be true (telemetry active)")
assert(st5.device_available == false or st5.device.available == false, "Scenario 5: device must be reported unavailable")
assert(st5.device.enabled == 0, "Scenario 5: device.enabled must be 0")
assert(mockBar.visible == true, "Scenario 5: bar widget must be visible")
assert(mockBar.text == "62°C", "Scenario 5: bar widget must show temperature 62°C, got: " .. tostring(mockBar.text))

-- Navegar para Desempenho
mockEnv.onTabPerformance()
local treePerf5 = mockPanel.tree
assert(treePerf5 ~= nil, "Scenario 5: mockPanel.tree must not be nil")
assert(hasLabelWithText(treePerf5, "card1 (dGPU) — AMD Radeon RX 9060 XT"), "Scenario 5: must display card name and dGPU model")
assert(not hasLabelWithText(treePerf5, "Temperatura do encapsulamento da APU (Edge)"), "Scenario 5: must not display APU thermal label on dGPU")

-- Navegar para Tela & Cor
mockEnv.onTabColor()
local treeColor5 = mockPanel.tree
assert(treeColor5 ~= nil, "Scenario 5: color tree must not be nil")
assert(hasLabelWithText(treeColor5, "Módulo /dev/chroma_amd não encontrado no kernel. Calibragem indisponível."), "Scenario 5: must warn that module is absent")
assert(not hasLabelWithText(treeColor5, "Calibragem: ATIVADA"), "Scenario 5: must NEVER display 'Calibragem: ATIVADA' when module is absent")
print("  [PASS] Scenario 5 passed.")

-- Cenário 6 — Concorrência: comandos imediatos em trânsito não são descartados
print("[test_ui] Running Scenario 6: Concurrent immediate set commands...")
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"available":true,"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","gpu_name":"AMD Radeon RX 9060 XT","gpu_type":"dGPU","pci_id":"0000:2d:00.0","device_id":"1002:7590","is_apu":false,"gpu_busy_percent":10,"sclk_mhz":1200,"vram_used_mib":2048.0,"vram_total_mib":8192.0,"gtt_used_mib":null,"gtt_total_mib":null,"temp_edge_c":45.0,"temp_junction_c":55.0,"power_watts":60.0}}]],
    stderr = "",
}
for _ = 1, 20 do
    mockEnv.update()
end

deferSetCommands = true
pendingAsyncCallbacks = {}

-- Disparar primeiro comando imediato (ex: saturação = 120)
mockEnv.noctalia.state.set("command", { action = "set", saturation = 120, immediate = true })
assert(#pendingAsyncCallbacks == 1, "Scenario 6: first set command must be in flight")
assert(string.find(pendingAsyncCallbacks[1].cmd, "-s 120", 1, true) ~= nil, "Scenario 6: first command must have sat=120")

-- Disparar segundo comando imediato enquanto o primeiro ainda está em trânsito (ex: saturação = 220)
mockEnv.noctalia.state.set("command", { action = "set", saturation = 220, immediate = true })
-- O segundo comando não deve ter sido descartado, mas também não deve ter disparado processo concorrente
assert(#pendingAsyncCallbacks == 1, "Scenario 6: second command must wait for first without running concurrently")

-- Concluir o primeiro comando
local firstInFlight = table.remove(pendingAsyncCallbacks, 1)
firstInFlight.cb({ exitCode = 0, stdout = "", stderr = "" })

-- Ao concluir o primeiro comando, o segundo deve ser drenado imediatamente
assert(#pendingAsyncCallbacks == 1, "Scenario 6: second command must be immediately drained after first finishes")
assert(string.find(pendingAsyncCallbacks[1].cmd, "-s 220", 1, true) ~= nil, "Scenario 6: drained command must carry the latest value (220)")

-- Concluir o segundo comando
local secondInFlight = table.remove(pendingAsyncCallbacks, 1)
deferSetCommands = false
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"available":true,"enabled":1,"saturation":220,"contrast":100,"brightness":0},"telemetry":{"card":"card1","gpu_name":"AMD Radeon RX 9060 XT","gpu_type":"dGPU","pci_id":"0000:2d:00.0","device_id":"1002:7590","is_apu":false,"gpu_busy_percent":10,"sclk_mhz":1200,"vram_used_mib":2048.0,"vram_total_mib":8192.0,"gtt_used_mib":null,"gtt_total_mib":null,"temp_edge_c":45.0,"temp_junction_c":55.0,"power_watts":60.0}}]],
    stderr = "",
}
secondInFlight.cb({ exitCode = 0, stdout = "", stderr = "" })

for _ = 1, 5 do
    mockEnv.update()
end
assert(mockState["status"].device.saturation == 220, "Scenario 6: final device saturation must be 220")
print("  [PASS] Scenario 6 passed.")

-- Cenário 7 — Rollback e liberação de hasPendingColorUpdate após falha em prisma-ctl set
print("[test_ui] Running Scenario 7: Rollback on prisma-ctl set failure...")
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"available":true,"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","gpu_name":"AMD Radeon RX 9060 XT","gpu_type":"dGPU","pci_id":"0000:2d:00.0","device_id":"1002:7590","is_apu":false,"gpu_busy_percent":10,"sclk_mhz":1200,"vram_used_mib":2048.0,"vram_total_mib":8192.0,"gtt_used_mib":null,"gtt_total_mib":null,"temp_edge_c":45.0,"temp_junction_c":55.0,"power_watts":60.0}}]],
    stderr = "",
}
for _ = 1, 20 do
    mockEnv.update()
end

mockEnv.onTabColor()
local treeColor7 = mockPanel.tree
assert(treeColor7 ~= nil, "Scenario 7: color tree must not be nil")
assert(hasLabelWithText(treeColor7, "100%"), "Scenario 7: initial saturation must be 100%")

-- Ajustar saturação localmente (ex: preset competitivo -> saturação 160)
deferSetCommands = true
pendingAsyncCallbacks = {}

mockEnv.onPresetCompetitive()
assert(#pendingAsyncCallbacks == 1, "Scenario 7: preset competitive must trigger set command")
assert(hasLabelWithText(mockPanel.tree, "160%"), "Scenario 7: panel must show optimistic 160%")

-- Enfileirar outro comando enquanto o primeiro está em andamento (isDirty = true)
mockEnv.noctalia.state.set("command", { action = "set", saturation = 180, immediate = false })

-- Simular falha na execução de prisma-ctl set (exitCode ~= 0)
local failedCmd = table.remove(pendingAsyncCallbacks, 1)
failedCmd.cb({ exitCode = 1, stdout = "", stderr = "Device I/O error" })

-- Ao falhar, isDirty deve ser zerado e nenhum segundo comando deve ser disparado com valor stale
assert(#pendingAsyncCallbacks == 0, "Scenario 7: no subsequent set command must be queued/dispatched on failure")
deferSetCommands = false

-- O painel deve limpar hasPendingColorUpdate e restaurar 100% (deviceState real)
local treeAfterError = mockPanel.tree
assert(hasLabelWithText(treeAfterError, "100%"), "Scenario 7: panel must restore real device state (100%) on error")
assert(not hasLabelWithText(treeAfterError, "160%"), "Scenario 7: panel must not retain failed optimistic state (160%)")

-- Múltiplos updates de poll não devem reintroduzir o valor otimista falhado
for _ = 1, 5 do
    mockEnv.update()
end
assert(hasLabelWithText(mockPanel.tree, "100%"), "Scenario 7: panel must remain at 100% after subsequent poll cycles")
assert(not hasLabelWithText(mockPanel.tree, "160%"), "Scenario 7: panel must not retain failed optimistic state")
print("  [PASS] Scenario 7 passed.")

-- Cenário 8 — onProfileNameChange atualiza botão Salvar imediatamente
print("[test_ui] Running Scenario 8: onProfileNameChange instant save button toggle...")
mockEnv.onTabProfiles()
local treeProfiles8 = mockPanel.tree
assert(treeProfiles8 ~= nil, "Scenario 8: profiles tree must not be nil")

local saveBtnsInitial = findNodes(treeProfiles8, function(n)
    return n.type == "button" and n.props.text == "Salvar"
end)
assert(#saveBtnsInitial == 1, "Scenario 8: 'Salvar' button must exist")
assert(saveBtnsInitial[1].props.enabled == false, "Scenario 8: 'Salvar' button must be disabled when name is empty")

-- Digitar um nome de perfil -> deve habilitar imediatamente sem chamar update()
mockEnv.onProfileNameChange("perfil_teste")
local treeProfilesTyped = mockPanel.tree
local saveBtnsTyped = findNodes(treeProfilesTyped, function(n)
    return n.type == "button" and n.props.text == "Salvar"
end)
assert(#saveBtnsTyped == 1, "Scenario 8: 'Salvar' button must exist after typing")
assert(saveBtnsTyped[1].props.enabled == true, "Scenario 8: 'Salvar' button must be enabled immediately after typing")

-- Limpar o nome -> deve desabilitar imediatamente sem chamar update()
mockEnv.onProfileNameChange("   ")
local treeProfilesCleared = mockPanel.tree
local saveBtnsCleared = findNodes(treeProfilesCleared, function(n)
    return n.type == "button" and n.props.text == "Salvar"
end)
assert(#saveBtnsCleared == 1, "Scenario 8: 'Salvar' button must exist after clearing")
assert(saveBtnsCleared[1].props.enabled == false, "Scenario 8: 'Salvar' button must be disabled immediately after clearing")
print("  [PASS] Scenario 8 passed.")

-- Cenário 9 — Teto dinâmico de potência (power_cap_watts) vs. fallbacks (65W APU / 300W dGPU)
print("[test_ui] Running Scenario 9: Dynamic power_cap_watts and fallbacks...")

-- Sub-cenário 9.1: dGPU com power_cap_watts real (ex: 180W)
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"available":true,"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","gpu_name":"AMD Radeon RX 9060 XT","gpu_type":"dGPU","pci_id":"0000:2d:00.0","device_id":"1002:7590","is_apu":false,"gpu_busy_percent":15,"sclk_mhz":1800,"vram_used_mib":2048.0,"vram_total_mib":8192.0,"gtt_used_mib":null,"gtt_total_mib":null,"temp_edge_c":48.0,"temp_junction_c":58.0,"power_watts":90.0,"power_cap_watts":180.0}}]],
    stderr = "",
}
for _ = 1, 20 do
    mockEnv.update()
end
mockEnv.onTabPerformance()
local tree9_1 = mockPanel.tree
assert(tree9_1 ~= nil, "Scenario 9.1: tree must not be nil")
assert(hasLabelWithText(tree9_1, "Teto: 180 W"), "Scenario 9.1: panel must display dynamic 'Teto: 180 W'")
local pbars9_1 = findProgressBars(tree9_1)
local foundPower9_1 = false
for _, p in ipairs(pbars9_1) do
    if math.abs(p.props.progress - (90 / 180)) < 0.005 then foundPower9_1 = true end
end
assert(foundPower9_1, "Scenario 9.1: power progress bar must reflect 90/180 ratio (0.5)")

-- Sub-cenário 9.2: APU com power_cap_watts real (ex: 35W)
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"available":true,"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","gpu_name":"AMD Ryzen 5 5600GT","gpu_type":"APU","pci_id":"0000:05:00.0","device_id":"1002:1638","is_apu":true,"gpu_busy_percent":10,"sclk_mhz":1200,"vram_used_mib":512.0,"vram_total_mib":512.0,"gtt_used_mib":1024.0,"gtt_total_mib":8192.0,"temp_edge_c":42.0,"temp_junction_c":null,"power_watts":14.0,"power_cap_watts":35.0}}]],
    stderr = "",
}
for _ = 1, 20 do
    mockEnv.update()
end
mockEnv.onTabPerformance()
local tree9_2 = mockPanel.tree
assert(tree9_2 ~= nil, "Scenario 9.2: tree must not be nil")
assert(hasLabelWithText(tree9_2, "Teto: 35 W"), "Scenario 9.2: panel must display dynamic 'Teto: 35 W' for APU")
local pbars9_2 = findProgressBars(tree9_2)
local foundPower9_2 = false
for _, p in ipairs(pbars9_2) do
    if math.abs(p.props.progress - (14 / 35)) < 0.005 then foundPower9_2 = true end
end
assert(foundPower9_2, "Scenario 9.2: power progress bar must reflect 14/35 ratio (0.4)")

-- Sub-cenário 9.3: Fallback dGPU quando power_cap_watts for ausente/null -> 300W
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"available":true,"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","gpu_name":"AMD Radeon RX 9060 XT","gpu_type":"dGPU","pci_id":"0000:2d:00.0","device_id":"1002:7590","is_apu":false,"gpu_busy_percent":15,"sclk_mhz":1800,"vram_used_mib":2048.0,"vram_total_mib":8192.0,"gtt_used_mib":null,"gtt_total_mib":null,"temp_edge_c":48.0,"temp_junction_c":58.0,"power_watts":60.0,"power_cap_watts":null}}]],
    stderr = "",
}
for _ = 1, 20 do
    mockEnv.update()
end
mockEnv.onTabPerformance()
local tree9_3 = mockPanel.tree
assert(tree9_3 ~= nil, "Scenario 9.3: tree must not be nil")
assert(hasLabelWithText(tree9_3, "Teto: 300 W"), "Scenario 9.3: panel must fallback to 'Teto: 300 W' when power_cap_watts is null")

-- Sub-cenário 9.4: Fallback APU quando power_cap_watts for ausente/null -> 65W
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"available":true,"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","gpu_name":"AMD Ryzen 5 5600GT","gpu_type":"APU","pci_id":"0000:05:00.0","device_id":"1002:1638","is_apu":true,"gpu_busy_percent":10,"sclk_mhz":1200,"vram_used_mib":512.0,"vram_total_mib":512.0,"gtt_used_mib":1024.0,"gtt_total_mib":8192.0,"temp_edge_c":42.0,"temp_junction_c":null,"power_watts":20.0}}]],
    stderr = "",
}
for _ = 1, 20 do
    mockEnv.update()
end
mockEnv.onTabPerformance()
local tree9_4 = mockPanel.tree
assert(tree9_4 ~= nil, "Scenario 9.4: tree must not be nil")
assert(hasLabelWithText(tree9_4, "Teto: 65 W"), "Scenario 9.4: panel must fallback to 'Teto: 65 W' when power_cap_watts is missing")

-- Sub-cenário 9.5: Guarda contra divisão por zero se sensor retornar valor fracionário baixo (ex: 0.3W)
mockStatusResponse = {
    exitCode = 0,
    stdout = [[{"device":{"available":true,"enabled":1,"saturation":100,"contrast":100,"brightness":0},"telemetry":{"card":"card1","gpu_name":"AMD Radeon RX 9060 XT","gpu_type":"dGPU","pci_id":"0000:2d:00.0","device_id":"1002:7590","is_apu":false,"gpu_busy_percent":5,"sclk_mhz":500,"vram_used_mib":2048.0,"vram_total_mib":8192.0,"gtt_used_mib":null,"gtt_total_mib":null,"temp_edge_c":40.0,"temp_junction_c":45.0,"power_watts":1.0,"power_cap_watts":0.3}}]],
    stderr = "",
}
for _ = 1, 20 do
    mockEnv.update()
end
mockEnv.onTabPerformance()
local tree9_5 = mockPanel.tree
assert(tree9_5 ~= nil, "Scenario 9.5: tree must not be nil")
assert(hasLabelWithText(tree9_5, "Teto: 1 W"), "Scenario 9.5: panel must clamp floor powerMaxW to at least 1 W")
local pbars9_5 = findProgressBars(tree9_5)
local foundPower9_5 = false
for _, p in ipairs(pbars9_5) do
    if math.abs(p.props.progress - 1.0) < 0.005 then foundPower9_5 = true end
end
assert(foundPower9_5, "Scenario 9.5: power progress bar must not divide by zero and reach 1.0 ratio")
print("  [PASS] Scenario 9 passed.")

print("[test_ui] Running profile JSON and active state tests...")
mockProfilesResponse.stdout = [=[
[
    {"brightness":0, "enabled":1, "name":"padrao", "contrast":100, "saturation":100},
    {"brightness":0, "enabled":0, "name":"desativado", "contrast":100, "saturation":100},
    {"brightness":-5, "enabled":1, "name":"cinema", "contrast":105, "saturation":120},
    false,
    {"name": {}, "saturation":100, "contrast":100, "brightness":0, "enabled":1},
    {"name":"invalido", "saturation":"texto", "contrast":100, "brightness":0, "enabled":1}
]
]=]
mockEnv.onTabProfiles()
local profiles = mockState.status.profiles
assert(#profiles == 3, "Only valid JSON profile entries should reach the panel")
assert(profiles[3].name == "cinema" and profiles[3].saturation == 120 and profiles[3].contrast == 105
    and profiles[3].brightness == -5 and profiles[3].enabled == 1, "Profile values should retain their numeric types")

local function assertProfileActive(name, active)
    local rows = findNodes(mockPanel.tree, function(n)
        return n.type == "row" and hasLabelWithText(n, name)
            and #findNodes(n, function(child) return child.props.glyph == "trash" end) == 1
    end)
    assert(#rows == 1, "Missing profile row for " .. name)
    local apply = findNodes(rows[1], function(n) return n.props.glyph == "check" end)[1]
    local delete = findNodes(rows[1], function(n) return n.props.glyph == "trash" end)[1]
    assert((apply.props.enabled == false) == active, "Unexpected apply state for " .. name)
    assert(delete.props.enabled ~= false and type(delete.props.onClick) == "function", "Deletion must remain available")
end
assertProfileActive("padrao", true)
assertProfileActive("desativado", false)
assertProfileActive("cinema", false)

for _, invalid in ipairs({ "[", '"invalid"', '{"name":"object"}' }) do
    mockProfilesResponse.stdout = invalid
    mockEnv.onTabProfiles()
    assert(#mockState.status.profiles == 3, "Invalid JSON or shape should preserve saved profiles")
end
mockProfilesResponse.stdout = "[]"
mockEnv.onTabProfiles()
assert(#mockState.status.profiles == 0, "An empty JSON list must clear saved profiles")

mockEnv.onTabColor()
local function checkPreset(sat, con, bri, enabled, available, expected)
    local status = table.clone(mockState.status)
    status.device = { saturation = sat, contrast = con, brightness = bri, enabled = enabled, available = available }
    noctalia.state.set("status", status)
    for _, name in ipairs({ "Padrão", "Competitivo", "Cinema", "Mono" }) do
        local buttons = findNodes(mockPanel.tree, function(n)
            return n.type == "button" and n.props.text == name
        end)
        assert(#buttons == 1, "Missing quick preset " .. name)
        assert(buttons[1].props.variant == (if name == expected then "primary" else "outline"),
            "Unexpected preset highlight for " .. name)
    end
end
checkPreset(100, 100, 0, 1, true, "Padrão")
checkPreset(160, 115, 10, 1, true, "Competitivo")
checkPreset(120, 105, -5, 1, true, "Cinema")
checkPreset(0, 110, 0, 1, true, "Mono")
checkPreset(160, 115, 9, 1, true, nil)
checkPreset(100, 100, 0, 0, true, nil)
checkPreset(100, 100, 0, 1, false, nil)
print("[test_ui] Running panel and widget localization tests...")
local function assertLanguage(performance, brightness)
    mockEnv.onTabPerformance()
    local tabs = findNodes(mockPanel.tree, function(n)
        return n.type == "button" and n.props.text == performance
    end)
    assert(#tabs == 1, "Performance tab must display " .. performance)
    mockEnv.onConfigChanged()
    assert(string.find(mockBar.tooltip, brightness, 1, true), "Missing translated widget value: " .. brightness)
end

language = "en"
assertLanguage("Performance", "Brightness: 0%")
language = "pt-BR"
assertLanguage("Desempenho", "Brilho: 0%")
language = "en"
noctalia.tr = nil
assertLanguage("Desempenho", "Brilho: 0%")
noctalia.tr = function(key) return key end
assertLanguage("Desempenho", "Brilho: 0%")
noctalia.tr = function() error("translation lookup failed") end
assertLanguage("Desempenho", "Brilho: 0%")

local function cliResponse(stdout)
    return { exitCode = 0, stdout = stdout, stderr = "" }
end

local function newAutomationSession(configuration, deviceAvailable, deferAutomation)
    local session = { state = {}, commands = {}, pending = {}, deferred = {} }
    local watchers = {}
    local windowCommand = "niri msg -j focused-window"
    local ruleList = "prisma-ctl rule list --json"
    session.responses = {
        [ruleList] = cliResponse(configuration),
        [windowCommand] = cliResponse("null"),
        ["prisma-ctl profile list --json"] = cliResponse([[
            [{"name":"desktop","saturation":130,"contrast":110,"brightness":-8,"enabled":0},
             {"name":"competitivo","saturation":160,"contrast":115,"brightness":10,"enabled":1}]
        ]]),
    }
    session.deferred[ruleList] = deferAutomation
    local api = table.clone(noctalia)
    api.tr = translate
    api.state = {
        get = function(key) return session.state[key] end,
        set = function(key, value)
            session.state[key] = value
            for _, callback in ipairs(watchers[key] or {}) do callback(value) end
        end,
        watch = function(key, callback)
            watchers[key] = watchers[key] or {}
            table.insert(watchers[key], callback)
        end,
    }
    api.runAsync = function(command, callback)
        table.insert(session.commands, command)
        if session.deferred[command] then
            table.insert(session.pending, { command = command, callback = callback })
        else
            callback(session.responses[command] or cliResponse(""))
        end
    end

    local env = {
        noctalia = api,
        ui = ui,
        panel = {
            render = function(tree) session.tree = tree end,
            close = function() end,
        },
    }
    setmetatable(env, { __index = getfenv() })
    session.env = env
    function session.setDevice(available, saturation, contrast, brightness, enabled)
        session.responses["prisma-ctl status --json"] = if available
            then cliResponse(string.format(
                [[{"device":{"available":true,"saturation":%d,"contrast":%d,"brightness":%d,"enabled":%d}}]],
                saturation or 130, contrast or 110, brightness or -8, enabled or 0))
            else { exitCode = 1, stdout = "", stderr = "Device unavailable" }
    end
    function session.focus(id, appId)
        session.responses[windowCommand] = cliResponse(
            if id then string.format([[{"id":%d,"title":"Window","app_id":"%s"}]], id, appId) else "null"
        )
    end
    function session.advance(ticks)
        for _ = 1, ticks do env.update() end
    end
    function session.count(command)
        local count = 0
        for _, ran in ipairs(session.commands) do
            if ran == command then count += 1 end
        end
        return count
    end
    function session.send(command)
        api.state.set("command", command)
    end
    session.setDevice(deviceAvailable)
    loadComponent("service", env)
    return session
end

local windowCommand = "niri msg -j focused-window"
local applyDefault = "prisma-ctl profile apply 'desktop'"
local applyGame = "prisma-ctl profile apply 'competitivo'"
local startupConfiguration = [[{"enabled":false,"default_profile":"desktop","rules":[]}]]
local gameConfiguration = [[{
    "enabled":true,"default_profile":"desktop",
    "rules":[{"pattern":"steam_app_*","profile":"desktop"},
             {"pattern":"steam_app_730","profile":"competitivo"}]
}]]
local snapshotConfiguration = [[{
    "enabled":true,"default_profile":null,"rules":[{"pattern":"steam_app_*","profile":"competitivo"}]
}]]

print("[test_ui] Running startup profile restoration tests...")
do
    local session = newAutomationSession(startupConfiguration, false)
    assert(session.state.status.automation.default_profile == "desktop")
    assert(session.count(applyDefault) == 0, "Startup must wait for the device")
    session.setDevice(true)
    session.advance(100)
    assert(session.count(applyDefault) == 1, "Startup must restore the default on the first successful device poll")
    session.send({ action = "rule_list" })
    session.advance(200)
    assert(session.count(applyDefault) == 1, "Startup restoration must run once per service session")
    assert(session.count(windowCommand) == 0, "Startup restoration must not require Niri or enabled rules")

    local delayed = newAutomationSession(startupConfiguration, true, true)
    assert(delayed.count(applyDefault) == 0, "Startup must wait for the automation configuration")
    assert(#delayed.pending == 1 and delayed.pending[1].command == "prisma-ctl rule list --json")
    delayed.pending[1].callback(delayed.responses["prisma-ctl rule list --json"])
    assert(delayed.count(applyDefault) == 1, "Default must be restored when configuration is loaded after device confirmation")

    local external = newAutomationSession(startupConfiguration, true)
    external.focus(42, "steam_app_730")
    external.responses["prisma-ctl rule list --json"] = cliResponse(gameConfiguration)
    external.advance(200)
    assert(external.state.status.automation.enabled, "The service must pick up CLI changes without opening the panel")
    assert(external.count(applyGame) == 1 and external.count(applyDefault) == 1)
end

print("[test_ui] Running Niri focus transitions and manual override tests...")
do
    local session = newAutomationSession(gameConfiguration, true)
    assert(session.count(applyDefault) == 1)
    session.focus(42, "STEAM_APP_730")
    session.deferred[windowCommand] = true
    session.advance(60)
    assert(session.count(windowCommand) == 1 and #session.pending == 1, "Only one Niri request may be in flight")
    session.deferred[windowCommand] = false
    table.remove(session.pending).callback(session.responses[windowCommand])
    assert(session.count(applyGame) == 1 and session.count(applyDefault) == 1, "Exact rule must beat the wildcard")
    assert(session.state.status.automation.active_rule == "steam_app_730")
    assert(session.state.status.automation.active_app_id == "STEAM_APP_730")
    session.advance(40)
    assert(session.count(applyGame) == 1, "Polling the same window must not reapply the rule")
    session.focus(nil)
    session.advance(20)
    assert(session.count(applyDefault) == 1, "A null focus must not restore the default profile")
    session.focus(11, "org.mozilla.firefox")
    session.advance(20)
    assert(session.count(applyDefault) == 2, "Focusing an unmatched real window must restore the default")
    assert(session.state.status.automation.active_rule == nil)
    assert(session.state.status.automation.last_app_id == "org.mozilla.firefox")

    local snapshot = newAutomationSession(snapshotConfiguration, true)
    snapshot.setDevice(true, 140, 105, -12, 1)
    snapshot.focus(42, "steam_app_570")
    snapshot.advance(20)
    assert(snapshot.count(applyGame) == 1, "Wildcard must apply to matching games")
    snapshot.send({ action = "set", saturation = 170, immediate = true })
    snapshot.advance(40)
    assert(snapshot.count(applyGame) == 1, "A manual change must take precedence on the same window")
    snapshot.focus(43, "steam_app_570")
    snapshot.advance(20)
    assert(snapshot.count(applyGame) == 2, "A different window ID must clear manual override even with the same app ID")
    snapshot.focus(11, "org.mozilla.firefox")
    snapshot.advance(20)
    assert(snapshot.count("prisma-ctl set -s 140 -c 105 -b -12 -e 1") == 1,
        "Without a default, leaving a rule must restore even changes made outside the service before the game")
end

print("[test_ui] Running Niri pause and fallback tests...")
do
    local session = newAutomationSession(snapshotConfiguration, true)
    session.send({ action = "panel_state", open = true, tab = "profiles" })
    session.focus(42, "steam_app_570")
    session.advance(40)
    assert(session.count(applyGame) == 0, "Automatic switching must pause while the panel is open")
    assert(session.state.status.automation.last_app_id == "steam_app_570", "Profiles must still capture an app ID")
    session.send({ action = "panel_state", open = false })
    session.advance(20)
    assert(session.count(applyGame) == 1, "The current focus must be reconciled when the panel closes")
    session.responses[windowCommand] = { exitCode = 127, stdout = "", stderr = "niri: command not found" }
    session.advance(20)
    local niriRequests = session.count(windowCommand)
    assert(not session.state.status.automation.niri_available)
    assert(session.state.status.error == nil, "Niri failure must not surface as a device error")
    session.advance(100)
    assert(session.count(windowCommand) == niriRequests, "Niri failure must stop background requests")

    local capture = newAutomationSession(startupConfiguration, true)
    capture.focus(8, "cs2")
    capture.send({ action = "panel_state", open = true, tab = "performance" })
    capture.advance(40)
    assert(capture.count(windowCommand) == 0, "Other tabs must not start focus capture when automation is disabled")
    capture.send({ action = "panel_state", open = true, tab = "profiles" })
    capture.advance(20)
    assert(capture.state.status.automation.last_app_id == "cs2", "Profiles must capture focus with automation disabled")

    local mutation = newAutomationSession(snapshotConfiguration, true)
    local userCommand = "prisma-ctl set -s 170 -c 110 -b -8 -e 0"
    mutation.deferred[userCommand] = true
    mutation.send({ action = "set", saturation = 170, immediate = true })
    mutation.focus(41, "steam_app_570")
    mutation.advance(20)
    assert(mutation.count(applyGame) == 0, "An in-flight mutation must pause automatic switching")
    assert(#mutation.pending == 1 and mutation.pending[1].command == userCommand)
    table.remove(mutation.pending).callback(cliResponse(""))
    mutation.advance(20)
    assert(mutation.count(applyGame) == 1, "A transition must reconcile after a mutation finishes")
end

print("[test_ui] Running Profiles automation control tests...")
do
    language = "en"
    local session = newAutomationSession(gameConfiguration, true)
    session.focus(42, "steam_app_730")
    session.advance(20)
    loadComponent("panel", session.env)
    session.env.onOpen()
    session.env.onTabProfiles()

    local function control(key)
        local nodes = findNodes(session.tree, function(node) return node.props.key == key end)
        assert(#nodes == 1, "Missing or duplicate control " .. key)
        return nodes[1].props
    end
    local function invoke(handler, ...)
        if type(handler) == "string" then handler = session.env[handler] end
        assert(type(handler) == "function", "Missing control callback")
        handler(...)
    end
    assert(hasLabelWithText(session.tree, translate("panel.automation.title")))
    assert(hasLabelWithText(session.tree, translate("panel.profiles.startup_default")))
    assert(hasLabelWithText(session.tree, translate("panel.automation.active", { pattern = "steam_app_730" })))
    assert(hasLabelWithText(session.tree, "steam_app_730 → competitivo"))
    assert(control("rule_pattern_0").placeholder == translate("panel.automation.pattern"))
    assert(control("rule_add").enabled == false, "An empty pattern must disable Add")
    invoke(control("profile_default_desktop").onClick)
    assert(session.state.command.action == "rule_clear_default")
    invoke(control("profile_default_competitivo").onClick)
    assert(session.state.command.action == "rule_set_default" and session.state.command.profile == "competitivo")
    invoke(control("rule_toggle").onClick)
    assert(session.state.command.action == "rule_toggle")
    invoke(control("rule_delete_steam_app_730").onClick)
    assert(session.state.command.action == "rule_delete" and session.state.command.pattern == "steam_app_730")

    invoke(control("rule_use_last_app").onClick)
    assert(control("rule_pattern_1").value == "steam_app_730", "Last detected app ID must populate the input")
    invoke(control("rule_pattern_1").onChange, "steam_app_*")
    invoke(control("rule_profile").onChange, "1", "competitivo")
    assert(control("rule_add").enabled ~= false)
    invoke(control("rule_add").onClick)
    local command = session.state.command
    assert(command.action == "rule_set" and command.pattern == "steam_app_*" and command.profile == "competitivo")
    session.responses[windowCommand] = { exitCode = 1, stdout = "", stderr = "Niri socket unavailable" }
    session.advance(20)
    assert(hasLabelWithText(session.tree, translate("panel.automation.niri_unavailable")))
end

print("[test_ui] Running saved profile listing with an older CLI...")
do
    language = "pt-BR"
    local profileList = "prisma-ctl profile list --json"
    local ruleList = "prisma-ctl rule list --json"
    local session = newAutomationSession(startupConfiguration, true)
    local supportedProfiles = session.responses[profileList]
    local supportedRules = session.responses[ruleList]
    session.responses[profileList] = cliResponse("[]")
    session.send({ action = "profile_list" })

    session.responses[profileList] = cliResponse(
        "Saved profiles (/home/user/.config/prisma/profiles):\n  - Padrão           [sat=200, con=115, bri=10, en=1]\n")
    session.responses[ruleList] = { exitCode = 1, stdout = "", stderr = "Unknown command: 'rule'\n\n" }
    loadComponent("panel", session.env)
    session.env.onOpen()
    session.env.onTabProfiles()

    local warning = translate("errors.cli_profiles_compatibility")
    assert(session.state.status.profiles_error == warning, "Plain-text profile output must explain the CLI mismatch")
    assert(session.state.status.automation.error == warning, "An unknown rule command must explain the CLI mismatch")
    assert(warning ~= "errors.cli_profiles_compatibility", "The compatibility warning must be translated")
    assert(hasLabelWithText(session.tree, warning))
    assert(not hasLabelWithText(session.tree, translate("panel.profiles.empty")), "A failed read cannot establish an empty profile list")
    session.advance(40)
    assert(session.state.status.profiles_error == warning and session.state.status.automation.error == warning,
        "Successful device polling must preserve the profile and automation errors")
    assert(session.state.status.error == nil, "A CLI compatibility warning must stay in Profiles")

    session.responses[profileList] = cliResponse("No saved profiles found in /home/user/.config/prisma/profiles\n")
    session.env.onRefreshProfilesClicked()
    assert(session.state.status.profiles_error == warning, "An older CLI reports an empty list as text as well")

    session.responses[profileList] = supportedProfiles
    session.env.onRefreshProfilesClicked()
    assert(session.state.status.profiles_error == nil)
    assert(hasLabelWithText(session.tree, "competitivo"), "A valid JSON list must make saved profiles visible again")
    assert(hasLabelWithText(session.tree, warning), "The remaining rule compatibility warning must still be visible")
    for _, key in ipairs({ "rule_toggle", "profile_default_competitivo" }) do
        local controls = findNodes(session.tree, function(node) return node.props.key == key end)
        assert(#controls == 1 and controls[1].props.enabled == false, key .. " must be disabled when rules cannot be read")
    end

    session.responses[ruleList] = supportedRules
    session.env.onRefreshProfilesClicked()
    assert(session.state.status.automation.error == nil)
    assert(not hasLabelWithText(session.tree, warning))
    assert(hasLabelWithText(session.tree, translate("panel.profiles.startup_default")))
end
local noRules = [[{"enabled":false,"rules":[]}]]

print("[test_ui] Running telemetry peak and reset tests...")
do
    local session = newAutomationSession(noRules, true)
    local statusCommand = "prisma-ctl status --json"
    local function sample(json)
        session.responses[statusCommand] = cliResponse('{"telemetry":' .. json .. '}')
        session.advance(100)
    end
    local function assertPeaks(load, temp, power, clock)
        local peaks = session.state.status.peaks
        assert(peaks.gpu_busy_max == load and peaks.temp_max == temp and peaks.power_max == power
            and peaks.sclk_max == clock, "Unexpected session maxima")
    end

    assertPeaks(0, 0, 0, 0)
    sample([[{"gpu_busy_percent":98,"temp_edge_c":54,"temp_junction_c":68.5,"sclk_mhz":2450}]])
    sample([[{"gpu_busy_percent":12,"temp_edge_c":65,"power_watts":42.5,"sclk_mhz":500}]])
    assertPeaks(98, 68.5, 42.5, 2450)
    session.advance(6000)
    assert(#session.state.status.history.gpu_busy == 60 and session.state.status.history.gpu_busy[1] == 12)
    assertPeaks(98, 68.5, 42.5, 2450)

    local previousPeak = session.state.status.peaks
    local polls = session.count(statusCommand)
    session.send({ action = "reset_peaks" })
    assertPeaks(0, 0, 0, 0)
    assert(previousPeak.gpu_busy_max == 98 and session.count(statusCommand) == polls)
    assert(#session.state.status.history.power == 60 and session.state.status.history.power[60] == 42.5)
    session.setDevice(true)
    session.advance(100)
    assertPeaks(0, 0, 0, 0)
    sample([[{"gpu_busy_percent":3,"temp_edge_c":40.5,"temp_junction_c":null,"power_watts":null,"sclk_mhz":300}]])
    assertPeaks(3, 40.5, 0, 300)
    assert(session.state.status.history.power[60] == 42.5, "Missing sensor must not append a fictitious zero")
end

print("[test_ui] Running performance chart and peak control tests...")
do
    language = "en"
    local session = newAutomationSession(noRules, true)
    loadComponent("panel", session.env)
    local status = table.clone(session.state.status)
    status.peaks = { gpu_busy_max = 98, temp_max = 68, power_max = 42, sclk_max = 2450 }
    status.telemetry = { is_apu = true, temp_edge_c = 42, power_cap_watts = 0 }
    local function charts(load, temp, power)
        status.history = { gpu_busy = load, temp_junction = temp, power = power }
        session.env.noctalia.state.set("status", status)
        local rows = findNodes(session.tree, function(node)
            return node.type == "row" and node.props.align == "end" and node.props.height == 24
        end)
        assert(#rows == 3, "Load, temperature and power must each have a chart")
        for _, row in ipairs(rows) do
            for _, bar in ipairs(row.children) do
                assert(bar.type == "box" and bar.props.height >= 2 and bar.props.height <= 24)
            end
        end
        return rows
    end
    for _, row in ipairs(charts({}, {}, {})) do assert(#row.children == 0) end
    local partial = charts({ -1, 50, 150 }, { 47.5, 95 }, { 32.5, 65 })
    for i, expected in ipairs({ { 2, 12, 24 }, { 12, 24 }, { 12, 24 } }) do
        assert(#partial[i].children == #expected)
        for j, height in ipairs(expected) do assert(partial[i].children[j].props.height == height) end
    end
    status.telemetry = { temp_junction_c = 68, power_cap_watts = 200 }
    local full = table.create(60, 0)
    for i = 31, 60 do full[i] = 50 end
    local rows = charts(full, { 55 }, { 100 })
    assert(#rows[1].children == 30, "Full history must show only the most recent 30 samples")
    for _, bar in ipairs(rows[1].children) do assert(bar.props.height == 12) end
    assert(rows[2].children[1].props.height == 12 and rows[3].children[1].props.height == 12)
    for _, peak in ipairs({ "Peak: 98%", "Peak: 68°C", "Peak: 42 W", "Peak: 2450 MHz" }) do
        assert(hasLabelWithText(session.tree, peak), peak)
    end
    local reset = findNodes(session.tree, function(node)
        return node.type == "button" and node.props.text == "Reset peaks"
    end)
    assert(#reset == 1 and reset[1].props.variant == "ghost")
    reset[1].props.onClick()
    assert(session.state.command.action == "reset_peaks" and hasLabelWithText(session.tree, "Peak: 0 W"))
end

print("[test_ui] Running bar widget state and pointer tests...")
do
    language = "en"
    local session = newAutomationSession(snapshotConfiguration, true)
    local widgetEnv = { noctalia = session.env.noctalia, barWidget = barWidget }
    setmetatable(widgetEnv, { __index = getfenv() })
    local openedPanel
    widgetEnv.noctalia.togglePanel = function(id) openedPanel = id end
    loadComponent("widget", widgetEnv)
    assert(mockBar.glyph == "player-pause" and mockBar.text == "")
    assert(mockBar.tooltip:find("Calibration: OFF", 1, true))
    widgetEnv.onRightClick()
    assert(session.count("prisma-ctl set -s 130 -c 110 -b -8 -e 1") == 1)

    session.responses["prisma-ctl status --json"] = cliResponse([[
        {"device":{"available":true,"enabled":1,"saturation":160,"contrast":115,"brightness":10},
         "telemetry":{"card":"card1","gpu_name":"Radeon","temp_junction_c":68,"temp_edge_c":50,
                      "gpu_busy_percent":98,"power_watts":42}}
    ]])
    session.focus(1, "steam_app_730")
    session.advance(100)
    assert(mockBar.glyph == "palette" and mockBar.glyphColor == "primary" and mockBar.text == "68°C")
    for _, text in ipairs({ "Calibration: ON", "Radeon", "GPU: 98%", "Hotspot: 68°C", "Power: 42W",
        "Saturation: 160%", "Contrast: 115%", "Brightness: 10%", "Niri rule: steam_app_*" }) do
        assert(mockBar.tooltip:find(text, 1, true), text)
    end
    widgetEnv.onClick()
    assert(openedPanel == "deyvideguilherme/prisma:panel")
    widgetEnv.onRightClick()
    assert(session.count("prisma-ctl set -s 160 -c 115 -b 10 -e 0") == 1)

    session.responses["prisma-ctl status --json"] = cliResponse([[
        {"device":{"available":false},"telemetry":{"card":"card1","temp_edge_c":36,"gpu_busy_percent":0}}
    ]])
    session.advance(100)
    assert(mockBar.glyph == "plug-off" and mockBar.text == "36°C" and mockBar.visible)
    mockConfig.bar_metric = "gpu_busy"
    widgetEnv.onConfigChanged()
    assert(mockBar.text == "0%", "A real zero load remains visible")
    local count = #session.commands
    widgetEnv.onRightClick()
    assert(#session.commands == count, "An offline module must not receive writes")
    session.setDevice(false)
    session.advance(100)
    assert(mockBar.glyph == "plug-off" and mockBar.text == "" and not mockBar.tooltip:find("36°C", 1, true))
    mockConfig.hide_inactive = true
    widgetEnv.onConfigChanged()
    assert(not mockBar.visible)
    mockConfig.hide_inactive, mockConfig.bar_metric = false, "hotspot"
end
print("All Luau UI behavioral tests PASSED successfully!")
