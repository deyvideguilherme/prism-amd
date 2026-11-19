--!nonstrict
-- Prisma Control Center - Control Center quick tile

local function applyStatus(status)
    local available = if type(status) == "table" and status.available then true else false
    local isEnabled = if type(status) == "table" and type(status.device) == "table" and status.device.enabled == 1 then true else false
    local autoProfile = if type(status) == "table" and type(status.activeAutoProfile) == "string" then status.activeAutoProfile else nil

    shortcut.setLabel(if autoProfile then `Prisma: {autoProfile}` else "Prisma")
    shortcut.setIcon("palette")
    shortcut.setActive(isEnabled)
    shortcut.setEnabled(available)
end

applyStatus(noctalia.state.get("status"))
noctalia.state.watch("status", applyStatus)

function onClick()
    noctalia.togglePanel("deyvideguilherme/prisma:panel")
end
