local ADDON_NAME = ...
if not ADDON_NAME or ADDON_NAME == "" then
    ADDON_NAME = "AzerothAutofarm"
end

local AF = CreateFrame("Frame", "AzerothAutofarmController")
_G.AzerothAutofarm = AF

local DATA = AzerothAutofarmData or { categories = {}, materials = {} }
local ROW_HEIGHT = 29
local ROW_COUNT = 9
local FALLBACK_ICON = "Interface\\Icons\\INV_Misc_QuestionMark"
local MAIN_FRAME_WIDTH = 860
local MAIN_FRAME_HEIGHT = 675
local LOG_FRAME_WIDTH = 700
local LOG_FRAME_HEIGHT = 560
local HELP_FRAME_WIDTH = 590
local HELP_FRAME_HEIGHT = 500
local FRAME_SCREEN_MARGIN = 32

local COLORS = {
    window = { 0.020, 0.025, 0.032, 0.985 },
    panel = { 0.034, 0.042, 0.052, 0.96 },
    panelAlt = { 0.044, 0.054, 0.066, 0.96 },
    border = { 0.20, 0.25, 0.30, 1.0 },
    borderSoft = { 0.13, 0.17, 0.21, 1.0 },
    accent = { 0.85, 0.64, 0.25, 1.0 },
    accentBright = { 1.0, 0.80, 0.37, 1.0 },
    green = { 0.31, 0.77, 0.46, 1.0 },
    greenDark = { 0.075, 0.23, 0.13, 1.0 },
    red = { 0.89, 0.31, 0.25, 1.0 },
    redDark = { 0.25, 0.065, 0.055, 1.0 },
    blue = { 0.38, 0.68, 0.94, 1.0 },
    text = { 0.92, 0.94, 0.97, 1.0 },
    muted = { 0.60, 0.67, 0.74, 1.0 },
    dim = { 0.39, 0.44, 0.50, 1.0 },
    rowOdd = { 0.045, 0.055, 0.067, 0.72 },
    rowEven = { 0.028, 0.035, 0.044, 0.72 },
    rowHover = { 0.85, 0.64, 0.25, 0.13 },
    rowSelected = { 0.31, 0.77, 0.46, 0.14 },
}

local BACKDROP = {
    bgFile = "Interface\\ChatFrame\\ChatFrameBackground",
    edgeFile = "Interface\\Buttons\\WHITE8X8",
    tile = true,
    tileSize = 16,
    edgeSize = 1,
    insets = { left = 1, right = 1, top = 1, bottom = 1 },
}

local WINDOW_BACKDROP = {
    bgFile = "Interface\\ChatFrame\\ChatFrameBackground",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true,
    tileSize = 16,
    edgeSize = 14,
    insets = { left = 4, right = 4, top = 4, bottom = 4 },
}

local DEFAULTS = {
    botName = "",
    quantity = "0",
    category = "mining",
    itemText = "2770",
    selectedItemId = 2770,
    favorites = {},
    frame = { point = "CENTER", relativePoint = "CENTER", x = 0, y = 0 },
    minimap = { shown = true, angle = 225 },
    activity = { autoRefresh = true, interval = 15 },
}

local function Trim(text)
    text = tostring(text or "")
    return text:match("^%s*(.-)%s*$") or ""
end

local function Lower(text)
    return string.lower(tostring(text or ""))
end

local function FormatDuration(seconds)
    seconds = math.max(0, tonumber(seconds) or 0)
    local hours = math.floor(seconds / 3600)
    local minutes = math.floor((seconds % 3600) / 60)
    local remaining = math.floor(seconds % 60)
    if hours > 0 then
        return string.format("%dh %02dm %02ds", hours, minutes, remaining)
    end
    return string.format("%dm %02ds", minutes, remaining)
end

local function FormatNumber(value)
    local number = tonumber(value) or 0
    if number >= 1000000 then
        return string.format("%.1fm", number / 1000000)
    elseif number >= 1000 then
        return string.format("%.1fk", number / 1000)
    end
    return tostring(math.floor(number))
end

local function CopyDefaults(target, defaults)
    for key, value in pairs(defaults) do
        if type(value) == "table" then
            if type(target[key]) ~= "table" then
                target[key] = {}
            end
            CopyDefaults(target[key], value)
        elseif target[key] == nil then
            target[key] = value
        end
    end
end

local function SetBackdrop(frame, background, border, backdrop)
    frame:SetBackdrop(backdrop or BACKDROP)
    frame:SetBackdropColor(background[1], background[2], background[3], background[4])
    frame:SetBackdropBorderColor(border[1], border[2], border[3], border[4])
end

local function SetTextureColor(texture, color)
    texture:SetVertexColor(color[1], color[2], color[3], color[4] or 1)
end

local function SetFontColor(fontString, color)
    fontString:SetTextColor(color[1], color[2], color[3], color[4] or 1)
end

local function FitFrameToScreen(frame, nativeWidth, nativeHeight)
    if not frame or not UIParent then
        return
    end

    local screenWidth = tonumber(UIParent:GetWidth()) or nativeWidth
    local screenHeight = tonumber(UIParent:GetHeight()) or nativeHeight
    local availableWidth = math.max(1, screenWidth - FRAME_SCREEN_MARGIN)
    local availableHeight = math.max(1, screenHeight - FRAME_SCREEN_MARGIN)
    local scale = math.min(1, availableWidth / nativeWidth, availableHeight / nativeHeight)
    frame:SetScale(math.max(0.5, scale))
end

local function GetMaterialById(itemId)
    itemId = tonumber(itemId)
    if not itemId then
        return nil
    end

    for _, material in ipairs(DATA.materials) do
        if material.id == itemId then
            return material
        end
    end

    return nil
end

local function GetItemId(text)
    text = Trim(text)
    return tonumber(text:match("item:(%d+)") or text:match("^(%d+)$"))
end

local function GetIcon(itemId)
    if not itemId then
        return FALLBACK_ICON
    end

    if GetItemIcon then
        local icon = GetItemIcon(itemId)
        if icon then
            return icon
        end
    end

    if GetItemInfo then
        local icon = select(10, GetItemInfo(itemId))
        if icon then
            return icon
        end
    end

    return FALLBACK_ICON
end

local function AddTooltip(frame, title, body)
    frame:HookScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetText(title, COLORS.accentBright[1], COLORS.accentBright[2], COLORS.accentBright[3])
        if body and body ~= "" then
            GameTooltip:AddLine(body, COLORS.text[1], COLORS.text[2], COLORS.text[3], true)
        end
        GameTooltip:Show()
    end)
    frame:HookScript("OnLeave", function()
        GameTooltip:Hide()
    end)
end

local function CreateButton(parent, text, width, height, style)
    style = style or "normal"
    local button = CreateFrame("Button", nil, parent)
    button:SetSize(width, height)
    button.style = style

    local background = COLORS.panelAlt
    local border = COLORS.border
    local hoverBorder = COLORS.accent
    if style == "primary" then
        background = COLORS.greenDark
        border = COLORS.green
        hoverBorder = COLORS.green
    elseif style == "danger" then
        background = COLORS.redDark
        border = COLORS.red
        hoverBorder = COLORS.red
    end

    button.normalBackground = background
    button.normalBorder = border
    button.hoverBorder = hoverBorder
    SetBackdrop(button, background, border)

    button.label = button:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    button.label:SetPoint("CENTER", 0, 1)
    button.label:SetText(text)
    SetFontColor(button.label, style == "primary" and COLORS.text or COLORS.text)

    button:SetScript("OnEnter", function(self)
        self:SetBackdropColor(
            self.normalBackground[1] + 0.035,
            self.normalBackground[2] + 0.035,
            self.normalBackground[3] + 0.035,
            self.normalBackground[4]
        )
        self:SetBackdropBorderColor(
            self.hoverBorder[1], self.hoverBorder[2], self.hoverBorder[3], self.hoverBorder[4]
        )
    end)
    button:SetScript("OnLeave", function(self)
        SetBackdrop(self, self.normalBackground, self.normalBorder)
        GameTooltip:Hide()
    end)
    button:SetScript("OnMouseDown", function(self)
        self.label:SetPoint("CENTER", 1, 0)
    end)
    button:SetScript("OnMouseUp", function(self)
        self.label:SetPoint("CENTER", 0, 1)
    end)

    function button:SetButtonText(value)
        self.label:SetText(value)
    end

    return button
end

local function CreateEditBox(parent, width, height, placeholder)
    local editBox = CreateFrame("EditBox", nil, parent)
    editBox:SetSize(width, height)
    editBox:SetAutoFocus(false)
    editBox:SetFontObject("ChatFontNormal")
    editBox:SetTextInsets(8, 8, 1, 0)
    editBox:SetMaxLetters(180)
    SetBackdrop(editBox, COLORS.window, COLORS.border)

    editBox.placeholder = editBox:CreateFontString(nil, "ARTWORK", "GameFontDisableSmall")
    editBox.placeholder:SetPoint("LEFT", 8, 0)
    editBox.placeholder:SetText(placeholder or "")
    editBox.placeholder:SetTextColor(COLORS.dim[1], COLORS.dim[2], COLORS.dim[3])

    editBox:SetScript("OnEscapePressed", function(self)
        self:ClearFocus()
    end)
    editBox:HookScript("OnEditFocusGained", function(self)
        self:SetBackdropBorderColor(COLORS.accent[1], COLORS.accent[2], COLORS.accent[3], 1)
    end)
    editBox:HookScript("OnEditFocusLost", function(self)
        self:SetBackdropBorderColor(COLORS.border[1], COLORS.border[2], COLORS.border[3], 1)
        if Trim(self:GetText()) == "" then
            self.placeholder:Show()
        end
    end)
    editBox:HookScript("OnTextChanged", function(self)
        if Trim(self:GetText()) == "" and not self:HasFocus() then
            self.placeholder:Show()
        else
            self.placeholder:Hide()
        end
    end)

    return editBox
end

function AF:AddLog(message, kind)
    message = tostring(message or "")
    local timestamp = date("%H:%M:%S")
    local color = COLORS.text
    if kind == "success" then
        color = COLORS.green
    elseif kind == "error" then
        color = COLORS.red
    elseif kind == "command" then
        color = COLORS.accentBright
    elseif kind == "info" then
        color = COLORS.blue
    end

    self.logs = self.logs or {}
    table.insert(self.logs, { text = "[" .. timestamp .. "] " .. message, color = color })
    while #self.logs > 200 do
        table.remove(self.logs, 1)
    end

    if self.logMessages then
        self.logMessages:AddMessage(
            "[" .. timestamp .. "] " .. message,
            color[1], color[2], color[3]
        )
        self.logMessages:ScrollToBottom()
    end
end

function AF:SetActivity(message, kind)
    message = tostring(message or "Ready")
    if self.activityText then
        self.activityText:SetText(message)
        if kind == "error" then
            SetFontColor(self.activityText, COLORS.red)
        elseif kind == "success" then
            SetFontColor(self.activityText, COLORS.green)
        else
            SetFontColor(self.activityText, COLORS.muted)
        end
    end

    if self.statusText then
        local label = "READY"
        local color = COLORS.green
        if kind == "working" then
            label = "SENT"
            color = COLORS.accentBright
        elseif kind == "error" then
            label = "ERROR"
            color = COLORS.red
        elseif kind == "success" then
            label = "ACTIVE"
            color = COLORS.green
        end
        self.statusText:SetText(label)
        SetFontColor(self.statusText, color)
        self.statusDot:SetVertexColor(color[1], color[2], color[3], 1)
    end
end

function AF:Print(message)
    DEFAULT_CHAT_FRAME:AddMessage(
        "|cffd9a441Azeroth|r |cff72d68bAutofarm:|r " .. tostring(message or "")
    )
end

function AF:CleanBotName()
    local botName = Trim(self.botBox and self.botBox:GetText() or self.db.botName)
    botName = botName:match("^([^%s]+)") or ""
    return botName
end

function AF:SendCommand(command, activity, silent)
    command = Trim(command):gsub("[\r\n]", " ")
    if command == "" then
        return
    end

    self.captureUntil = GetTime() + 10
    if not silent then
        self:AddLog(command, "command")
        self:SetActivity(activity or "Command sent; waiting for the server...", "working")
    end
    SendChatMessage(command, "SAY")
end

function AF:StartFarm()
    local itemText = Trim(self.itemBox:GetText()):gsub("[\r\n]", " ")
    if itemText == "" then
        self:SetActivity("Choose a material or enter an item name, ID, or link.", "error")
        self:Print("Choose a material before starting.")
        return
    end

    local quantityText = Trim(self.quantityBox:GetText())
    local quantity = tonumber(quantityText == "" and "0" or quantityText)
    if not quantity or quantity < 0 or quantity ~= math.floor(quantity) then
        self:SetActivity("Quantity must be a whole number, or zero for unlimited.", "error")
        return
    end

    local botName = self:CleanBotName()
    local command
    if botName ~= "" then
        command = ".autofarm startbot " .. botName .. " " .. itemText
    else
        command = ".autofarm start " .. itemText
    end

    if quantity > 0 then
        command = command .. " --count " .. quantity
    end

    self.db.botName = botName
    self.db.quantity = tostring(quantity)
    self.db.itemText = itemText
    self.statusUnavailable = false
    self:SendCommand(command, "Starting farm route; waiting for the server...")
end

function AF:RequestStatus(silent)
    local botName = self:CleanBotName()
    local command = ".autofarm statusui"
    if botName ~= "" then
        command = command .. " " .. botName
    end
    self.statusUnavailable = false
    self.statusRequestPendingUntil = GetTime() + 8
    self:SendCommand(command, "Requesting detailed farming status...", silent)
end

function AF:StopFarm()
    local botName = self:CleanBotName()
    local command = ".autofarm stop"
    if botName ~= "" then
        command = command .. " " .. botName
    end
    self.statusUnavailable = true
    self:SendCommand(command, "Stopping the selected farming session...")
end

function AF:StopAll()
    self.statusUnavailable = true
    self:SendCommand(".autofarm stopall", "Stopping all owned farming sessions...")
end

function AF:SearchServer()
    local itemText = Trim(self.itemBox:GetText()):gsub("[\r\n]", " ")
    if itemText == "" then
        self:SetActivity("Enter part of an item name before searching the server.", "error")
        return
    end

    local itemId = GetItemId(itemText)
    local material = GetMaterialById(itemId)
    local linkName = itemText:match("%[(.-)%]")
    local query = material and material.name
        or linkName
        or itemText:gsub("|c%x%x%x%x%x%x%x%x", ""):gsub("|r", "")
    query = query:gsub("|H.-|h", ""):gsub("|h", "")
    self:SendCommand(".autofarm search " .. query, "Searching the server item list...")
end

function AF:UseTarget()
    if not UnitExists("target") or not UnitIsPlayer("target") then
        self:SetActivity("Target an online playerbot character first.", "error")
        return
    end

    local name = UnitName("target")
    if not name or name == "" then
        self:SetActivity("The selected target does not have a usable character name.", "error")
        return
    end

    self.botBox:SetText(name)
    self.botBox:ClearFocus()
    self.db.botName = name
    self:SetActivity("Bot set to " .. name .. ".", "success")
end

function AF:SetCategory(category)
    self.db.category = category
    if self.searchBox then
        self.searchBox:SetText("")
        self.searchBox:ClearFocus()
    end
    self:RefreshCategories()
    self:RefreshMaterials(true)
end

function AF:ToggleFavorite(itemId)
    local key = tostring(itemId)
    if self.db.favorites[key] then
        self.db.favorites[key] = nil
    else
        self.db.favorites[key] = true
    end
    self:RefreshCategories()
    self:RefreshMaterials(false)
end

function AF:SelectMaterial(material)
    if not material then
        return
    end

    self.db.selectedItemId = material.id
    self.db.itemText = tostring(material.id)
    self.itemBox:SetText(tostring(material.id))
    self.itemBox:ClearFocus()
    self:UpdateSelectedItem()
    self:RefreshMaterials(false)
    self:SetActivity(material.name .. " selected.", nil)
end

function AF:UpdateSelectedItem()
    if not self.itemBox then
        return
    end

    local text = Trim(self.itemBox:GetText())
    local itemId = GetItemId(text)
    local material = GetMaterialById(itemId)
    self.selectedIcon:SetTexture(GetIcon(itemId))

    if material then
        self.db.selectedItemId = material.id
        self.selectedName:SetText(material.name)
        self.selectedMeta:SetText(material.tier .. "  •  item " .. material.id)
        SetFontColor(self.selectedName, COLORS.text)
    elseif itemId then
        local itemName = GetItemInfo and GetItemInfo(itemId)
        self.selectedName:SetText(itemName or "Custom item " .. itemId)
        self.selectedMeta:SetText("Server item ID " .. itemId)
        SetFontColor(self.selectedName, COLORS.text)
    elseif text ~= "" then
        self.selectedName:SetText("Custom item search")
        self.selectedMeta:SetText("The server will resolve this exact name or link")
        SetFontColor(self.selectedName, COLORS.accentBright)
    else
        self.selectedName:SetText("No material selected")
        self.selectedMeta:SetText("Choose a preset or enter an item")
        SetFontColor(self.selectedName, COLORS.muted)
    end

    self.db.itemText = text
end

function AF:GetFilteredMaterials()
    local category = self.db.category or "mining"
    local query = Lower(Trim(self.searchBox and self.searchBox:GetText() or ""))
    local result = {}

    for _, material in ipairs(DATA.materials) do
        local categoryMatches = category == "all" or material.category == category
        if category == "favorites" then
            categoryMatches = self.db.favorites[tostring(material.id)] and true or false
        end

        local queryMatches = true
        if query ~= "" then
            categoryMatches = true
            queryMatches = Lower(material.name):find(query, 1, true) ~= nil
                or tostring(material.id):find(query, 1, true) ~= nil
                or Lower(material.tier):find(query, 1, true) ~= nil
        end

        if categoryMatches and queryMatches then
            table.insert(result, material)
        end
    end

    return result
end

function AF:RefreshCategories()
    if not self.categoryButtons then
        return
    end

    local counts = { all = #DATA.materials, favorites = 0 }
    for _, material in ipairs(DATA.materials) do
        counts[material.category] = (counts[material.category] or 0) + 1
    end
    for _, favorite in pairs(self.db.favorites) do
        if favorite then
            counts.favorites = counts.favorites + 1
        end
    end

    for _, button in ipairs(self.categoryButtons) do
        button.count:SetText(counts[button.category] or 0)
        if button.category == self.db.category then
            button.active:Show()
            button:SetBackdropColor(0.085, 0.075, 0.045, 0.98)
            SetFontColor(button.label, COLORS.accentBright)
        else
            button.active:Hide()
            button:SetBackdropColor(COLORS.panel[1], COLORS.panel[2], COLORS.panel[3], 0.80)
            SetFontColor(button.label, COLORS.muted)
        end
    end
end

function AF:RefreshMaterials(resetScroll)
    if not self.materialScroll then
        return
    end

    self.filteredMaterials = self:GetFilteredMaterials()
    if resetScroll then
        FauxScrollFrame_SetOffset(self.materialScroll, 0)
        if self.materialScrollScrollBar then
            self.materialScrollScrollBar:SetValue(0)
        end
    end

    FauxScrollFrame_Update(self.materialScroll, #self.filteredMaterials, ROW_COUNT, ROW_HEIGHT)
    local offset = FauxScrollFrame_GetOffset(self.materialScroll)

    for rowIndex, row in ipairs(self.materialRows) do
        local material = self.filteredMaterials[offset + rowIndex]
        row.material = material
        if material then
            row:Show()
            row.icon:SetTexture(GetIcon(material.id))
            row.name:SetText(material.name)
            row.tier:SetText(material.tier)
            row.itemId:SetText(material.id)
            row.star:SetText(self.db.favorites[tostring(material.id)] and "★" or "☆")
            if self.db.favorites[tostring(material.id)] then
                SetFontColor(row.star, COLORS.accentBright)
            else
                SetFontColor(row.star, COLORS.dim)
            end

            local selected = self.db.selectedItemId == material.id
            if selected then
                row:SetBackdropColor(
                    COLORS.rowSelected[1], COLORS.rowSelected[2], COLORS.rowSelected[3], COLORS.rowSelected[4]
                )
                row.selected:Show()
            else
                local rowColor = rowIndex % 2 == 0 and COLORS.rowEven or COLORS.rowOdd
                row:SetBackdropColor(rowColor[1], rowColor[2], rowColor[3], rowColor[4])
                row.selected:Hide()
            end
        else
            row:Hide()
        end
    end

    self.resultCount:SetText(#self.filteredMaterials .. " presets")
    if #self.filteredMaterials == 0 then
        self.emptyText:Show()
        if self.db.category == "favorites" then
            self.emptyText:SetText("No favorites yet\nRight-click any material to add one")
        else
            self.emptyText:SetText("No materials match this search")
        end
    else
        self.emptyText:Hide()
    end
end

function AF:SaveFramePosition()
    local point, _, relativePoint, x, y = self.mainFrame:GetPoint(1)
    self.db.frame.point = point or "CENTER"
    self.db.frame.relativePoint = relativePoint or "CENTER"
    self.db.frame.x = math.floor((x or 0) + 0.5)
    self.db.frame.y = math.floor((y or 0) + 0.5)
end

function AF:RefreshActivityDashboard()
    if not self.activityBotName then
        return
    end

    local status = self.activityStatus
    if not status then
        self.activityBotName:SetText("No telemetry received")
        self.activityState:SetText("WAITING")
        SetFontColor(self.activityState, COLORS.muted)
        self.activityMaterial:SetText("—")
        self.activityProgress:SetText("—")
        self.activityRoute:SetText("—")
        self.activityLocation:SetText("—")
        self.activitySource:SetText("—")
        self.activityVitals:SetText("—")
        self.activityTime:SetText("—")
        self.activityMovement:SetText("—")
        self.activityUpdated:SetText("Open Activity or click Refresh to request a snapshot")
        return
    end

    local state = status.state or "Unknown"
    local stateColor = COLORS.blue
    local lowerState = Lower(state)
    if lowerState:find("gather", 1, true) or lowerState:find("loot", 1, true) then
        stateColor = COLORS.green
    elseif lowerState:find("combat", 1, true) or lowerState:find("corpse", 1, true) then
        stateColor = COLORS.red
    elseif lowerState:find("recovery", 1, true) or lowerState:find("teleport", 1, true) then
        stateColor = COLORS.accentBright
    end

    self.activityBotName:SetText((status.bot or "Unknown bot") .. "  •  Level " .. (status.level or "?"))
    self.activityState:SetText(string.upper(state))
    SetFontColor(self.activityState, stateColor)

    local goal = tonumber(status.goal) or 0
    local goalText = goal > 0 and FormatNumber(goal) or "Unlimited"
    local distance = tonumber(status.distance) or -1
    local distanceText = distance >= 0 and string.format("%.1f yd", distance) or "Different map"
    local movementText = status.moving == "1" and "Moving" or "Standing"
    local recoveries = tonumber(status.recoveries) or 0
    local stalled = tonumber(status.stalled) or 0

    self.activityMaterial:SetText((status.item or "Unknown") .. "  •  item " .. (status.itemid or "?"))
    self.activityProgress:SetText(
        FormatNumber(status.gained) .. " / " .. goalText .. " gained  •  "
        .. FormatNumber(status.inventory) .. " carried"
    )
    self.activityRoute:SetText(
        "Point " .. (status.route or "?") .. " / " .. (status.routes or "?")
        .. "  •  " .. FormatNumber(status.loops) .. " loops"
    )
    self.activityLocation:SetText(
        (status.farmzone or "Unknown farm zone") .. "  •  now " .. (status.area or "Unknown area")
        .. "  •  map " .. (status.map or "?")
    )
    self.activitySource:SetText((status.source or "Material source") .. "  •  " .. distanceText)
    self.activityVitals:SetText(
        (status.health or "0") .. "% health  •  " .. FormatNumber(status.free) .. " free bag slots"
    )
    self.activityTime:SetText(
        FormatDuration(status.elapsed) .. " elapsed  •  " .. FormatNumber(status.rate) .. " items/hour"
    )
    self.activityMovement:SetText(
        movementText .. "  •  " .. recoveries .. " path recoveries  •  " .. stalled .. "s since progress"
    )
    self.activityUpdated:SetText("Last server snapshot " .. date("%H:%M:%S") .. "  •  15-second lightweight refresh")
end

function AF:HandleStatusTelemetry(message)
    if not tostring(message or ""):find("^AFSTATUS|", 1) then
        return false
    end

    local status = {}
    for key, value in tostring(message):gmatch("|([^=|]+)=([^|]*)") do
        status[key] = value
    end

    status.receivedAt = GetTime()
    self.activityStatus = status
    self.statusRequestPendingUntil = nil
    self.statusUnavailable = false
    self:RefreshActivityDashboard()

    local summaryKey = table.concat({
        status.bot or "", status.state or "", status.gained or "", status.route or "", status.recoveries or ""
    }, "|")
    if summaryKey ~= self.lastStatusLogKey then
        self.lastStatusLogKey = summaryKey
        self:AddLog(
            (status.bot or "Bot") .. ": " .. (status.state or "Unknown")
            .. ", " .. FormatNumber(status.gained) .. " gathered, route "
            .. (status.route or "?") .. "/" .. (status.routes or "?"),
            "success"
        )
    end

    self:SetActivity(
        (status.bot or "Bot") .. " — " .. (status.state or "active")
        .. ", " .. FormatNumber(status.gained) .. " gathered",
        "success"
    )
    return true
end

function AF:CreateLogFrame()
    local frame = CreateFrame("Frame", "AzerothAutofarmLogFrame", UIParent)
    frame:SetSize(LOG_FRAME_WIDTH, LOG_FRAME_HEIGHT)
    frame:SetPoint("CENTER", UIParent, "CENTER", 70, 15)
    frame:SetFrameStrata("DIALOG")
    frame:SetClampedToScreen(true)
    SetBackdrop(frame, COLORS.window, COLORS.accent, WINDOW_BACKDROP)
    frame:Hide()

    local header = frame:CreateTexture(nil, "BACKGROUND")
    header:SetTexture("Interface\\ChatFrame\\ChatFrameBackground")
    header:SetPoint("TOPLEFT", 5, -5)
    header:SetPoint("TOPRIGHT", -5, -5)
    header:SetHeight(42)
    header:SetVertexColor(0.075, 0.065, 0.035, 0.96)

    local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    title:SetPoint("TOPLEFT", 18, -16)
    title:SetText("Autofarm Activity Monitor")
    SetFontColor(title, COLORS.accentBright)

    local refresh = CreateButton(frame, "Refresh", 78, 25)
    refresh:SetPoint("TOPRIGHT", -189, -12)
    refresh:SetScript("OnClick", function()
        self.activityPollElapsed = 0
        self.statusUnavailable = false
        self:RequestStatus(false)
    end)
    AddTooltip(refresh, "Refresh now", "Requests one lightweight status snapshot from the server.")

    local auto = CreateButton(frame, "Auto: On", 88, 25)
    auto:SetPoint("TOPRIGHT", -95, -12)
    auto:SetScript("OnClick", function(selfButton)
        self.db.activity.autoRefresh = not self.db.activity.autoRefresh
        selfButton:SetButtonText(self.db.activity.autoRefresh and "Auto: On" or "Auto: Off")
        self.activityPollElapsed = 0
        if self.db.activity.autoRefresh then
            self.statusUnavailable = false
            self:RequestStatus(true)
        end
    end)
    AddTooltip(auto, "Automatic snapshots", "Polls every 15 seconds only while this window is open.")
    self.activityAutoButton = auto

    local close = CreateButton(frame, "×", 28, 25, "danger")
    close:SetPoint("TOPRIGHT", -12, -12)
    close.label:SetFont("Fonts\\FRIZQT__.TTF", 18, "OUTLINE")
    close:SetScript("OnClick", function()
        frame:Hide()
    end)

    local statusPanel = CreateFrame("Frame", nil, frame)
    statusPanel:SetPoint("TOPLEFT", 18, -56)
    statusPanel:SetSize(664, 226)
    SetBackdrop(statusPanel, COLORS.panel, COLORS.borderSoft)

    local botName = statusPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    botName:SetPoint("TOPLEFT", 13, -12)
    botName:SetText("No telemetry received")
    SetFontColor(botName, COLORS.text)
    self.activityBotName = botName

    local state = statusPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    state:SetPoint("TOPRIGHT", -13, -15)
    state:SetText("WAITING")
    SetFontColor(state, COLORS.muted)
    self.activityState = state

    local function CreateMetric(labelText, x, y, width)
        local label = statusPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        label:SetPoint("TOPLEFT", x, y)
        label:SetText(labelText)
        SetFontColor(label, COLORS.dim)

        local value = statusPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        value:SetPoint("TOPLEFT", x, y - 15)
        value:SetWidth(width)
        value:SetJustifyH("LEFT")
        value:SetText("—")
        SetFontColor(value, COLORS.text)
        return value
    end

    self.activityMaterial = CreateMetric("MATERIAL", 13, -47, 305)
    self.activityProgress = CreateMetric("PROGRESS", 336, -47, 315)
    self.activityRoute = CreateMetric("ROUTE", 13, -87, 305)
    self.activityLocation = CreateMetric("LOCATION", 336, -87, 315)
    self.activitySource = CreateMetric("CURRENT SOURCE", 13, -127, 305)
    self.activityVitals = CreateMetric("HEALTH AND BAGS", 336, -127, 315)
    self.activityTime = CreateMetric("SESSION", 13, -167, 305)
    self.activityMovement = CreateMetric("MOVEMENT", 336, -167, 315)

    local updated = statusPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    updated:SetPoint("BOTTOMLEFT", 13, 8)
    updated:SetPoint("BOTTOMRIGHT", -13, 8)
    updated:SetJustifyH("LEFT")
    updated:SetText("Open Activity or click Refresh to request a snapshot")
    SetFontColor(updated, COLORS.dim)
    self.activityUpdated = updated

    local logTitle = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    logTitle:SetPoint("TOPLEFT", 20, -294)
    logTitle:SetText("RECENT COMMANDS AND SERVER EVENTS")
    SetFontColor(logTitle, COLORS.muted)

    local messages = CreateFrame("ScrollingMessageFrame", "AzerothAutofarmLogMessages", frame)
    messages:SetPoint("TOPLEFT", 18, -314)
    messages:SetPoint("BOTTOMRIGHT", -42, 48)
    messages:SetFontObject("ChatFontNormal")
    messages:SetJustifyH("LEFT")
    messages:SetFading(false)
    messages:SetMaxLines(200)
    messages:EnableMouseWheel(true)
    messages:SetScript("OnMouseWheel", function(self, delta)
        if delta > 0 then
            self:ScrollUp()
        else
            self:ScrollDown()
        end
    end)

    local up = CreateButton(frame, "▲", 24, 24)
    up:SetPoint("TOPRIGHT", -12, -315)
    up:SetScript("OnClick", function()
        messages:ScrollUp()
    end)
    local down = CreateButton(frame, "▼", 24, 24)
    down:SetPoint("BOTTOMRIGHT", -12, 50)
    down:SetScript("OnClick", function()
        messages:ScrollDown()
    end)

    local clear = CreateButton(frame, "Clear", 74, 26)
    clear:SetPoint("BOTTOMLEFT", 18, 14)
    clear:SetScript("OnClick", function()
        self.logs = {}
        messages:Clear()
    end)
    local bottom = CreateButton(frame, "Jump to latest", 110, 26)
    bottom:SetPoint("BOTTOMRIGHT", -18, 14)
    bottom:SetScript("OnClick", function()
        messages:ScrollToBottom()
    end)

    frame:SetMovable(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", function(self)
        self:StartMoving()
    end)
    frame:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
    end)
    frame:SetScript("OnShow", function()
        self:UpdateFrameScales()
        self.activityPollElapsed = 0
        self.statusUnavailable = false
        self.activityAutoButton:SetButtonText(self.db.activity.autoRefresh and "Auto: On" or "Auto: Off")
        self:RefreshActivityDashboard()
        self:RequestStatus(true)
    end)
    frame:SetScript("OnUpdate", function(_, elapsed)
        if not self.db.activity.autoRefresh or self.statusUnavailable then
            return
        end

        self.activityPollElapsed = (self.activityPollElapsed or 0) + elapsed
        local interval = math.max(10, tonumber(self.db.activity.interval) or 15)
        if self.activityPollElapsed < interval then
            return
        end
        if self.statusRequestPendingUntil and GetTime() < self.statusRequestPendingUntil then
            return
        end

        self.activityPollElapsed = 0
        self:RequestStatus(true)
    end)

    self.logFrame = frame
    self.logMessages = messages
    table.insert(UISpecialFrames, "AzerothAutofarmLogFrame")
end

function AF:ToggleLog()
    if self.logFrame:IsShown() then
        self.logFrame:Hide()
        return
    end

    self.logMessages:Clear()
    for _, entry in ipairs(self.logs or {}) do
        self.logMessages:AddMessage(entry.text, entry.color[1], entry.color[2], entry.color[3])
    end
    self.logMessages:ScrollToBottom()
    self.logFrame:Show()
end

function AF:CreateHelpFrame()
    local frame = CreateFrame("Frame", "AzerothAutofarmHelpFrame", UIParent)
    frame:SetSize(HELP_FRAME_WIDTH, HELP_FRAME_HEIGHT)
    frame:SetPoint("CENTER")
    frame:SetFrameStrata("DIALOG")
    frame:SetClampedToScreen(true)
    SetBackdrop(frame, COLORS.window, COLORS.accent, WINDOW_BACKDROP)
    frame:Hide()
    frame:SetScript("OnShow", function()
        self:UpdateFrameScales()
    end)

    local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    title:SetPoint("TOPLEFT", 22, -21)
    title:SetText("Azeroth Autofarm — Quick Guide")
    SetFontColor(title, COLORS.accentBright)

    local close = CreateButton(frame, "×", 28, 25, "danger")
    close:SetPoint("TOPRIGHT", -14, -14)
    close.label:SetFont("Fonts\\FRIZQT__.TTF", 18, "OUTLINE")
    close:SetScript("OnClick", function()
        frame:Hide()
    end)

    local guide = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    guide:SetPoint("TOPLEFT", 24, -62)
    guide:SetPoint("BOTTOMRIGHT", -24, 55)
    guide:SetJustifyH("LEFT")
    guide:SetJustifyV("TOP")
    guide:SetSpacing(4)
    guide:SetText(
        "|cff72d68b1. Choose the playerbot|r\n"
        .. "Target an online playerbot and click Use Target, or type its exact character name. Leave the name blank "
        .. "to use the playerbot you currently have selected.\n\n"
        .. "|cff72d68b2. Choose a material|r\n"
        .. "Browse categories, search all presets, or enter an exact item name, item ID, or shift-clicked item link. "
        .. "Right-click a preset to add or remove it from Favorites.\n\n"
        .. "|cff72d68b3. Set the goal|r\n"
        .. "The quantity is how many new items to gather during this session. Use zero for unlimited farming.\n\n"
        .. "|cff72d68b4. Start and monitor|r\n"
        .. "Start Farming sends the request to mod-autofarm. Status checks progress. Stop returns that bot according "
        .. "to the server configuration; Stop All ends every farming session you own. Open Activity for a detailed "
        .. "altbot dashboard. It requests one lightweight server snapshot every 15 seconds only while open, and its "
        .. "Auto button can disable polling.\n\n"
        .. "|cffffcc5cImportant|r\n"
        .. "The bot must already be online through mod-playerbots and must know the required gathering profession. "
        .. "It also needs tools such as a mining pick or skinning knife. Fishing presets only work when the item has "
        .. "an outdoor fishing-school source. Crafted-only, vendor-only, container-only, and open-water-only items may "
        .. "be rejected by the server.\n\n"
        .. "|cff8fbfffShortcuts|r\n"
        .. "/autofarm or /afarm — toggle this window\n"
        .. "/afarm log — open the activity log\n"
        .. "Left-click the minimap button — toggle window\n"
        .. "Right-click the minimap button — request status"
    )

    local done = CreateButton(frame, "Got it", 100, 28, "primary")
    done:SetPoint("BOTTOM", 0, 17)
    done:SetScript("OnClick", function()
        frame:Hide()
    end)

    self.helpFrame = frame
    table.insert(UISpecialFrames, "AzerothAutofarmHelpFrame")
end

function AF:UpdateMinimapPosition()
    if not self.minimapButton then
        return
    end

    local angle = math.rad(tonumber(self.db.minimap.angle) or 225)
    self.minimapButton:ClearAllPoints()
    self.minimapButton:SetPoint("CENTER", Minimap, "CENTER", 80 * math.cos(angle), 80 * math.sin(angle))
    if self.db.minimap.shown then
        self.minimapButton:Show()
    else
        self.minimapButton:Hide()
    end
end

function AF:CreateMinimapButton()
    local button = CreateFrame("Button", "AzerothAutofarmMinimapButton", Minimap)
    button:SetSize(32, 32)
    button:SetFrameStrata("MEDIUM")
    button:SetFrameLevel(8)
    button:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    button:RegisterForDrag("LeftButton")

    local icon = button:CreateTexture(nil, "BACKGROUND")
    icon:SetTexture("Interface\\Icons\\Trade_Mining")
    icon:SetSize(20, 20)
    icon:SetPoint("CENTER")
    icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)

    local border = button:CreateTexture(nil, "OVERLAY")
    border:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
    border:SetSize(54, 54)
    border:SetPoint("TOPLEFT", -11, 11)

    local highlight = button:CreateTexture(nil, "HIGHLIGHT")
    highlight:SetTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")
    highlight:SetBlendMode("ADD")
    highlight:SetSize(32, 32)
    highlight:SetPoint("CENTER")

    button:SetScript("OnClick", function(_, mouseButton)
        if mouseButton == "RightButton" then
            self:RequestStatus()
        else
            self:Toggle()
        end
    end)
    button:SetScript("OnDragStart", function(selfButton)
        selfButton:SetScript("OnUpdate", function()
            local cursorX, cursorY = GetCursorPosition()
            local scale = UIParent:GetEffectiveScale()
            local centerX, centerY = Minimap:GetCenter()
            cursorX = cursorX / scale
            cursorY = cursorY / scale
            local angle = math.deg(math.atan2(cursorY - centerY, cursorX - centerX))
            self.db.minimap.angle = angle
            self:UpdateMinimapPosition()
        end)
    end)
    button:SetScript("OnDragStop", function(selfButton)
        selfButton:SetScript("OnUpdate", nil)
    end)
    AddTooltip(button, "Azeroth Autofarm", "Left-click to open. Right-click to request bot status. Drag to move.")

    self.minimapButton = button
    self:UpdateMinimapPosition()
end

function AF:CreateMainFrame()
    local frame = CreateFrame("Frame", "AzerothAutofarmFrame", UIParent)
    frame:SetSize(MAIN_FRAME_WIDTH, MAIN_FRAME_HEIGHT)
    frame:SetFrameStrata("DIALOG")
    frame:SetClampedToScreen(true)
    frame:SetMovable(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    SetBackdrop(frame, COLORS.window, COLORS.accent, WINDOW_BACKDROP)
    frame:SetPoint(
        self.db.frame.point,
        UIParent,
        self.db.frame.relativePoint,
        self.db.frame.x,
        self.db.frame.y
    )
    frame:SetScript("OnDragStart", function(selfFrame)
        selfFrame:StartMoving()
    end)
    frame:SetScript("OnDragStop", function(selfFrame)
        selfFrame:StopMovingOrSizing()
        self:SaveFramePosition()
    end)
    frame:SetScript("OnShow", function()
        self:UpdateFrameScales()
    end)
    frame:Hide()

    local header = frame:CreateTexture(nil, "BACKGROUND")
    header:SetTexture("Interface\\ChatFrame\\ChatFrameBackground")
    header:SetPoint("TOPLEFT", 5, -5)
    header:SetPoint("TOPRIGHT", -5, -5)
    header:SetHeight(54)
    header:SetVertexColor(0.075, 0.064, 0.033, 0.98)

    local headerLine = frame:CreateTexture(nil, "ARTWORK")
    headerLine:SetTexture("Interface\\Buttons\\WHITE8X8")
    headerLine:SetPoint("BOTTOMLEFT", header, "BOTTOMLEFT", 0, 0)
    headerLine:SetPoint("BOTTOMRIGHT", header, "BOTTOMRIGHT", 0, 0)
    headerLine:SetHeight(1)
    SetTextureColor(headerLine, COLORS.accent)

    local logo = frame:CreateTexture(nil, "ARTWORK")
    logo:SetTexture("Interface\\Icons\\Trade_Mining")
    logo:SetSize(39, 39)
    logo:SetPoint("TOPLEFT", 16, -12)
    logo:SetTexCoord(0.07, 0.93, 0.07, 0.93)

    local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    title:SetPoint("TOPLEFT", 64, -13)
    title:SetText("Azeroth Autofarm")
    SetFontColor(title, COLORS.accentBright)

    local subtitle = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    subtitle:SetPoint("TOPLEFT", 65, -36)
    subtitle:SetText("Material routing and playerbot control")
    SetFontColor(subtitle, COLORS.muted)

    local statusDot = frame:CreateTexture(nil, "OVERLAY")
    statusDot:SetTexture("Interface\\Buttons\\WHITE8X8")
    statusDot:SetSize(7, 7)
    statusDot:SetPoint("TOPRIGHT", -174, -27)
    statusDot:SetVertexColor(COLORS.green[1], COLORS.green[2], COLORS.green[3], 1)
    self.statusDot = statusDot

    local statusText = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    statusText:SetPoint("LEFT", statusDot, "RIGHT", 6, 0)
    statusText:SetText("READY")
    SetFontColor(statusText, COLORS.green)
    self.statusText = statusText

    local logButton = CreateButton(frame, "Activity", 72, 25)
    logButton:SetPoint("TOPRIGHT", -87, -18)
    logButton:SetScript("OnClick", function()
        self:ToggleLog()
    end)
    AddTooltip(logButton, "Activity log", "View server replies and commands sent by this addon.")

    local helpButton = CreateButton(frame, "?", 28, 25)
    helpButton:SetPoint("TOPRIGHT", -53, -18)
    helpButton:SetScript("OnClick", function()
        self.helpFrame:Show()
    end)
    AddTooltip(helpButton, "Quick guide", "Open setup, farming, and command help.")

    local closeButton = CreateButton(frame, "×", 28, 25, "danger")
    closeButton:SetPoint("TOPRIGHT", -18, -18)
    closeButton.label:SetFont("Fonts\\FRIZQT__.TTF", 18, "OUTLINE")
    closeButton:SetScript("OnClick", function()
        frame:Hide()
    end)

    -- Category sidebar.
    local sidebar = CreateFrame("Frame", nil, frame)
    sidebar:SetPoint("TOPLEFT", 14, -70)
    sidebar:SetSize(160, 565)
    SetBackdrop(sidebar, COLORS.panel, COLORS.borderSoft)

    local categoriesTitle = sidebar:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    categoriesTitle:SetPoint("TOPLEFT", 12, -12)
    categoriesTitle:SetText("MATERIAL GROUPS")
    SetFontColor(categoriesTitle, COLORS.muted)

    self.categoryButtons = {}
    for index, category in ipairs(DATA.categories) do
        local button = CreateFrame("Button", nil, sidebar)
        button:SetPoint("TOPLEFT", 8, -34 - ((index - 1) * 31))
        button:SetSize(144, 28)
        button.category = category.key
        SetBackdrop(button, COLORS.panel, COLORS.borderSoft)

        button.active = button:CreateTexture(nil, "ARTWORK")
        button.active:SetTexture("Interface\\Buttons\\WHITE8X8")
        button.active:SetPoint("TOPLEFT", 0, 0)
        button.active:SetPoint("BOTTOMLEFT", 0, 0)
        button.active:SetWidth(3)
        SetTextureColor(button.active, COLORS.accentBright)

        button.label = button:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        button.label:SetPoint("LEFT", 10, 0)
        button.label:SetText(category.name)
        button.label:SetJustifyH("LEFT")

        button.count = button:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        button.count:SetPoint("RIGHT", -8, 0)

        button:SetScript("OnClick", function(selfButton)
            self:SetCategory(selfButton.category)
        end)
        button:SetScript("OnEnter", function(selfButton)
            if selfButton.category ~= self.db.category then
                selfButton:SetBackdropBorderColor(
                    COLORS.accent[1], COLORS.accent[2], COLORS.accent[3], COLORS.accent[4]
                )
            end
        end)
        button:SetScript("OnLeave", function(selfButton)
            selfButton:SetBackdropBorderColor(
                COLORS.borderSoft[1], COLORS.borderSoft[2], COLORS.borderSoft[3], COLORS.borderSoft[4]
            )
        end)
        table.insert(self.categoryButtons, button)
    end

    local sidebarTip = sidebar:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    sidebarTip:SetPoint("BOTTOMLEFT", 11, 13)
    sidebarTip:SetPoint("BOTTOMRIGHT", -11, 13)
    sidebarTip:SetJustifyH("LEFT")
    sidebarTip:SetText("Right-click a material\nto toggle Favorites")
    SetFontColor(sidebarTip, COLORS.dim)

    -- Bot and quantity panel.
    local targetPanel = CreateFrame("Frame", nil, frame)
    targetPanel:SetPoint("TOPLEFT", 184, -70)
    targetPanel:SetSize(662, 98)
    SetBackdrop(targetPanel, COLORS.panel, COLORS.borderSoft)

    local targetTitle = targetPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    targetTitle:SetPoint("TOPLEFT", 14, -12)
    targetTitle:SetText("PLAYERBOT")
    SetFontColor(targetTitle, COLORS.muted)

    local botBox = CreateEditBox(targetPanel, 210, 29, "Blank = selected playerbot")
    botBox:SetPoint("TOPLEFT", 14, -33)
    botBox:SetText(self.db.botName or "")
    botBox:SetScript("OnEnterPressed", function(selfBox)
        selfBox:ClearFocus()
        self.db.botName = Trim(selfBox:GetText())
    end)
    self.botBox = botBox

    local targetButton = CreateButton(targetPanel, "Use Target", 92, 29)
    targetButton:SetPoint("LEFT", botBox, "RIGHT", 8, 0)
    targetButton:SetScript("OnClick", function()
        self:UseTarget()
    end)
    AddTooltip(targetButton, "Use current target", "Copies the targeted player character into the bot-name field.")

    local quantityTitle = targetPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    quantityTitle:SetPoint("TOPLEFT", 354, -12)
    quantityTitle:SetText("NEW ITEMS TO GATHER")
    SetFontColor(quantityTitle, COLORS.muted)

    local quantityBox = CreateEditBox(targetPanel, 72, 29, "0")
    quantityBox:SetPoint("TOPLEFT", 354, -33)
    quantityBox:SetNumeric(true)
    quantityBox:SetMaxLetters(7)
    quantityBox:SetText(self.db.quantity or "0")
    quantityBox:SetScript("OnEnterPressed", function(selfBox)
        selfBox:ClearFocus()
    end)
    self.quantityBox = quantityBox

    local quickCounts = {
        { label = "20", value = "20" },
        { label = "100", value = "100" },
        { label = "200", value = "200" },
        { label = "∞", value = "0" },
    }
    for index, quick in ipairs(quickCounts) do
        local button = CreateButton(targetPanel, quick.label, 48, 29)
        button:SetPoint("TOPLEFT", 434 + ((index - 1) * 53), -33)
        button:SetScript("OnClick", function()
            quantityBox:SetText(quick.value)
            quantityBox:ClearFocus()
        end)
        if quick.value == "0" then
            AddTooltip(button, "Unlimited", "Farm continuously until you press Stop.")
        end
    end

    local targetHint = targetPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    targetHint:SetPoint("BOTTOMLEFT", 14, 11)
    targetHint:SetText("The bot must already be online through mod-playerbots. Zero means unlimited farming.")
    SetFontColor(targetHint, COLORS.dim)

    -- Material browser panel.
    local materialPanel = CreateFrame("Frame", nil, frame)
    materialPanel:SetPoint("TOPLEFT", 184, -178)
    materialPanel:SetSize(662, 348)
    SetBackdrop(materialPanel, COLORS.panel, COLORS.borderSoft)

    local searchBox = CreateEditBox(materialPanel, 300, 28, "Search every preset by name, tier, or ID")
    searchBox:SetPoint("TOPLEFT", 14, -14)
    searchBox:SetScript("OnEnterPressed", function(selfBox)
        selfBox:ClearFocus()
    end)
    searchBox:HookScript("OnTextChanged", function()
        self:RefreshMaterials(true)
    end)
    self.searchBox = searchBox

    local clearSearch = CreateButton(materialPanel, "Clear", 62, 28)
    clearSearch:SetPoint("LEFT", searchBox, "RIGHT", 7, 0)
    clearSearch:SetScript("OnClick", function()
        searchBox:SetText("")
        searchBox:ClearFocus()
    end)

    local resultCount = materialPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    resultCount:SetPoint("RIGHT", -16, 0)
    resultCount:SetPoint("TOP", 0, -24)
    resultCount:SetJustifyH("RIGHT")
    SetFontColor(resultCount, COLORS.muted)
    self.resultCount = resultCount

    local columnLine = materialPanel:CreateTexture(nil, "ARTWORK")
    columnLine:SetTexture("Interface\\Buttons\\WHITE8X8")
    columnLine:SetPoint("TOPLEFT", 14, -51)
    columnLine:SetPoint("TOPRIGHT", -14, -51)
    columnLine:SetHeight(1)
    columnLine:SetVertexColor(COLORS.border[1], COLORS.border[2], COLORS.border[3], 0.8)

    local materialScroll = CreateFrame(
        "ScrollFrame", "AzerothAutofarmMaterialScroll", materialPanel, "FauxScrollFrameTemplate"
    )
    materialScroll:SetPoint("TOPLEFT", 14, -57)
    materialScroll:SetPoint("BOTTOMRIGHT", -31, 17)
    materialScroll:SetScript("OnVerticalScroll", function(selfScroll, offset)
        FauxScrollFrame_OnVerticalScroll(selfScroll, offset, ROW_HEIGHT, function()
            self:RefreshMaterials(false)
        end)
    end)
    self.materialScroll = materialScroll
    self.materialScrollScrollBar = _G.AzerothAutofarmMaterialScrollScrollBar

    self.materialRows = {}
    for rowIndex = 1, ROW_COUNT do
        local row = CreateFrame("Button", nil, materialPanel)
        row:SetPoint("TOPLEFT", 14, -58 - ((rowIndex - 1) * ROW_HEIGHT))
        row:SetSize(615, ROW_HEIGHT - 1)
        row:RegisterForClicks("LeftButtonUp", "RightButtonUp")
        SetBackdrop(row, rowIndex % 2 == 0 and COLORS.rowEven or COLORS.rowOdd, COLORS.borderSoft)

        row.selected = row:CreateTexture(nil, "ARTWORK")
        row.selected:SetTexture("Interface\\Buttons\\WHITE8X8")
        row.selected:SetPoint("TOPLEFT", 0, 0)
        row.selected:SetPoint("BOTTOMLEFT", 0, 0)
        row.selected:SetWidth(3)
        SetTextureColor(row.selected, COLORS.green)

        row.icon = row:CreateTexture(nil, "ARTWORK")
        row.icon:SetSize(23, 23)
        row.icon:SetPoint("LEFT", 7, 0)
        row.icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)

        row.name = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        row.name:SetPoint("LEFT", 38, 0)
        row.name:SetWidth(320)
        row.name:SetJustifyH("LEFT")
        SetFontColor(row.name, COLORS.text)

        row.tier = row:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        row.tier:SetPoint("LEFT", 382, 0)
        row.tier:SetWidth(86)
        row.tier:SetJustifyH("LEFT")
        SetFontColor(row.tier, COLORS.muted)

        row.itemId = row:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        row.itemId:SetPoint("RIGHT", -39, 0)
        row.itemId:SetWidth(70)
        row.itemId:SetJustifyH("RIGHT")
        SetFontColor(row.itemId, COLORS.dim)

        row.star = row:CreateFontString(nil, "OVERLAY", "GameFontNormal")
        row.star:SetPoint("RIGHT", -10, 0)
        row.star:SetWidth(20)
        row.star:SetJustifyH("CENTER")

        row:SetScript("OnClick", function(selfRow, mouseButton)
            if mouseButton == "RightButton" then
                self:ToggleFavorite(selfRow.material.id)
            else
                self:SelectMaterial(selfRow.material)
            end
        end)
        row:SetScript("OnEnter", function(selfRow)
            selfRow:SetBackdropBorderColor(
                COLORS.accent[1], COLORS.accent[2], COLORS.accent[3], COLORS.accent[4]
            )
            GameTooltip:SetOwner(selfRow, "ANCHOR_RIGHT")
            GameTooltip:SetText(selfRow.material.name)
            GameTooltip:AddLine(
                "Item " .. selfRow.material.id .. " • " .. selfRow.material.tier,
                COLORS.muted[1], COLORS.muted[2], COLORS.muted[3]
            )
            GameTooltip:AddLine("Left-click to select. Right-click to favorite.", 1, 1, 1, true)
            GameTooltip:Show()
        end)
        row:SetScript("OnLeave", function(selfRow)
            selfRow:SetBackdropBorderColor(
                COLORS.borderSoft[1], COLORS.borderSoft[2], COLORS.borderSoft[3], COLORS.borderSoft[4]
            )
            GameTooltip:Hide()
        end)
        table.insert(self.materialRows, row)
    end

    local emptyText = materialPanel:CreateFontString(nil, "OVERLAY", "GameFontDisable")
    emptyText:SetPoint("CENTER", 0, -15)
    emptyText:SetJustifyH("CENTER")
    emptyText:Hide()
    self.emptyText = emptyText

    -- Item selection and actions.
    local actionPanel = CreateFrame("Frame", nil, frame)
    actionPanel:SetPoint("TOPLEFT", 184, -536)
    actionPanel:SetSize(662, 99)
    SetBackdrop(actionPanel, COLORS.panel, COLORS.borderSoft)

    local itemTitle = actionPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    itemTitle:SetPoint("TOPLEFT", 14, -10)
    itemTitle:SetText("ITEM NAME, ID, OR LINK")
    SetFontColor(itemTitle, COLORS.muted)

    local itemBox = CreateEditBox(actionPanel, 298, 29, "Choose a preset or enter a custom item")
    itemBox:SetPoint("TOPLEFT", 14, -29)
    itemBox:SetText(self.db.itemText or "2770")
    itemBox:SetScript("OnEnterPressed", function()
        self:StartFarm()
    end)
    itemBox:HookScript("OnTextChanged", function()
        self:UpdateSelectedItem()
    end)
    self.itemBox = itemBox

    local searchServer = CreateButton(actionPanel, "Search Server", 101, 29)
    searchServer:SetPoint("LEFT", itemBox, "RIGHT", 7, 0)
    searchServer:SetScript("OnClick", function()
        self:SearchServer()
    end)
    AddTooltip(searchServer, "Search server items", "Uses .autofarm search for names not listed in the presets.")

    local selectedIcon = actionPanel:CreateTexture(nil, "ARTWORK")
    selectedIcon:SetSize(27, 27)
    selectedIcon:SetPoint("TOPLEFT", 14, -64)
    selectedIcon:SetTexCoord(0.07, 0.93, 0.07, 0.93)
    self.selectedIcon = selectedIcon

    local selectedName = actionPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    selectedName:SetPoint("TOPLEFT", 49, -64)
    selectedName:SetWidth(220)
    selectedName:SetJustifyH("LEFT")
    self.selectedName = selectedName

    local selectedMeta = actionPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    selectedMeta:SetPoint("TOPLEFT", 49, -80)
    selectedMeta:SetWidth(300)
    selectedMeta:SetJustifyH("LEFT")
    SetFontColor(selectedMeta, COLORS.dim)
    self.selectedMeta = selectedMeta

    local startButton = CreateButton(actionPanel, "Start Farming", 126, 33, "primary")
    startButton:SetPoint("TOPRIGHT", -14, -16)
    startButton:SetScript("OnClick", function()
        self:StartFarm()
    end)
    AddTooltip(startButton, "Start farming", "Build a route and begin farming the selected material.")

    local statusButton = CreateButton(actionPanel, "Status", 75, 28)
    statusButton:SetPoint("BOTTOMRIGHT", -14, 12)
    statusButton:SetScript("OnClick", function()
        self:RequestStatus()
    end)

    local stopButton = CreateButton(actionPanel, "Stop", 75, 28, "danger")
    stopButton:SetPoint("RIGHT", statusButton, "LEFT", -7, 0)
    stopButton:SetScript("OnClick", function()
        self:StopFarm()
    end)

    local stopAllButton = CreateButton(actionPanel, "Stop All", 82, 28, "danger")
    stopAllButton:SetPoint("RIGHT", stopButton, "LEFT", -7, 0)
    stopAllButton:SetScript("OnClick", function()
        StaticPopup_Show("AZEROTH_AUTOFARM_STOP_ALL")
    end)

    local footer = CreateFrame("Frame", nil, frame)
    footer:SetPoint("BOTTOMLEFT", 14, 10)
    footer:SetPoint("BOTTOMRIGHT", -14, 10)
    footer:SetHeight(22)
    SetBackdrop(footer, COLORS.panel, COLORS.borderSoft)

    local activityText = footer:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    activityText:SetPoint("LEFT", 8, 0)
    activityText:SetPoint("RIGHT", -8, 0)
    activityText:SetHeight(13)
    activityText:SetJustifyH("LEFT")
    activityText:SetWordWrap(false)
    activityText:SetText("Ready — choose a playerbot and material")
    SetFontColor(activityText, COLORS.muted)
    self.activityText = activityText

    self.mainFrame = frame
    table.insert(UISpecialFrames, "AzerothAutofarmFrame")
end

function AF:UpdateFrameScales()
    FitFrameToScreen(self.mainFrame, MAIN_FRAME_WIDTH, MAIN_FRAME_HEIGHT)
    FitFrameToScreen(self.logFrame, LOG_FRAME_WIDTH, LOG_FRAME_HEIGHT)
    FitFrameToScreen(self.helpFrame, HELP_FRAME_WIDTH, HELP_FRAME_HEIGHT)
end

function AF:HandleSystemMessage(message)
    if self:HandleStatusTelemetry(message) then
        return
    end

    local lower = Lower(message)
    local relevant = GetTime() <= (self.captureUntil or 0)
        or lower:find("autofarm", 1, true)
        or lower:find(" is farming [", 1, true)
        or lower:find("route points", 1, true)
        or lower:find("item matches for", 1, true)
        or lower:find("incidental profession", 1, true)

    if not relevant then
        return
    end

    local kind = "info"
    if lower:find("started", 1, true) or lower:find(" is farming [", 1, true) then
        kind = "success"
        self:SetActivity(message, "success")
    elseif lower:find("stopped", 1, true) then
        kind = "success"
        self:SetActivity(message, nil)
    elseif lower:find("no ", 1, true)
        or lower:find("does not", 1, true)
        or lower:find("cannot", 1, true)
        or lower:find("could not", 1, true)
        or lower:find("disabled", 1, true)
        or lower:find("invalid", 1, true)
        or lower:find("not found", 1, true)
        or lower:find("not an online", 1, true)
        or lower:find("only control", 1, true)
    then
        kind = "error"
        if lower:find("active autofarm session", 1, true)
            or lower:find("online playerbot", 1, true)
            or lower:find("not an online playerbot", 1, true)
        then
            self.statusUnavailable = true
            if self.activityState then
                self.activityState:SetText("NO ACTIVE SESSION")
                SetFontColor(self.activityState, COLORS.red)
                self.activityUpdated:SetText("Automatic refresh paused — start farming or click Refresh")
            end
        end
        self:SetActivity(message, "error")
    else
        self:SetActivity(message, nil)
    end

    self:AddLog(message, kind)
end

function AF:Toggle()
    if self.mainFrame:IsShown() then
        self.mainFrame:Hide()
    else
        self.mainFrame:Show()
        self:RefreshCategories()
        self:RefreshMaterials(false)
        self:UpdateSelectedItem()
    end
end

function AF:Initialize()
    AzerothAutofarmDB = AzerothAutofarmDB or {}
    CopyDefaults(AzerothAutofarmDB, DEFAULTS)
    self.db = AzerothAutofarmDB
    self.logs = {}

    self:CreateLogFrame()
    self:CreateHelpFrame()
    self:CreateMainFrame()
    self:CreateMinimapButton()
    self:UpdateFrameScales()
    self:RefreshCategories()
    self:RefreshMaterials(true)
    self:UpdateSelectedItem()

    StaticPopupDialogs["AZEROTH_AUTOFARM_STOP_ALL"] = {
        text = "Stop every autofarm session owned by this character?",
        button1 = YES,
        button2 = NO,
        OnAccept = function()
            AF:StopAll()
        end,
        timeout = 0,
        whileDead = true,
        hideOnEscape = true,
        preferredIndex = 3,
    }

    SLASH_AZEROTHAUTOFARM1 = "/autofarm"
    SLASH_AZEROTHAUTOFARM2 = "/afarm"
    SlashCmdList.AZEROTHAUTOFARM = function(message)
        local command = Lower(Trim(message))
        if command == "log" then
            AF:ToggleLog()
        elseif command == "help" then
            AF.helpFrame:Show()
        elseif command == "status" then
            AF:RequestStatus()
        elseif command == "minimap" then
            AF.db.minimap.shown = not AF.db.minimap.shown
            AF:UpdateMinimapPosition()
        else
            AF:Toggle()
        end
    end

    if hooksecurefunc and ChatEdit_InsertLink then
        hooksecurefunc("ChatEdit_InsertLink", function(link)
            if AF.itemBox and AF.itemBox:HasFocus() and link then
                AF.itemBox:SetText(link)
                AF.itemBox:SetCursorPosition(AF.itemBox:GetNumLetters())
            end
        end)
    end

    if ChatFrame_AddMessageEventFilter then
        ChatFrame_AddMessageEventFilter("CHAT_MSG_SYSTEM", function(_, _, message)
            if tostring(message or ""):find("^AFSTATUS|", 1) then
                return true
            end
            return false
        end)
    end

    self:RegisterEvent("CHAT_MSG_SYSTEM")
    self:RegisterEvent("GET_ITEM_INFO_RECEIVED")
    self:RegisterEvent("DISPLAY_SIZE_CHANGED")
    self:Print("loaded. Type |cffffcc5c/autofarm|r or click the minimap button.")
end

AF:SetScript("OnEvent", function(self, event, ...)
    if event == "ADDON_LOADED" then
        local loadedAddon = ...
        if loadedAddon == ADDON_NAME then
            self:UnregisterEvent("ADDON_LOADED")
            self:Initialize()
        end
    elseif event == "CHAT_MSG_SYSTEM" then
        self:HandleSystemMessage(...)
    elseif event == "GET_ITEM_INFO_RECEIVED" then
        self:RefreshMaterials(false)
        self:UpdateSelectedItem()
    elseif event == "DISPLAY_SIZE_CHANGED" then
        self:UpdateFrameScales()
    end
end)

AF:RegisterEvent("ADDON_LOADED")
