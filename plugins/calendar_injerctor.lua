-- F1 2012 Calendar Injector (Enhanced & Optimized)
-- Intercepts both race arrays and creates a custom F1 calendar with support for up to 24 races
-- Modified to write current race position to track+70h

(function()
    -- All variables isolated within this function
    local initialized = false
	local initialized2 = false
    local trackDatabase = {}  -- Will store found tracks
    local customCalendar = {}  -- Will store the user calendar
    local originalArray = {start = 0, end_ = 0, size = 0}  -- Original array
    local customArray = {address = 0, size = 0}  -- Our first custom array (track pointers)
    local customStructure = {address = 0, size = 0}  -- Our second custom array (full structure)

    -- Function to write to log (clear log on startup)
    local function writeLog(message, clearLog)
        local mode = clearLog and "w" or "a"
        local logFile = io.open("calendar_injector.log", mode)
        if logFile then
            logFile:write(os.date("%Y-%m-%d %H:%M:%S") .. ": " .. message .. "\n")
            logFile:close()
        end
    end

    -- Clear log file at startup
    writeLog("Starting new session", true)

    -- Optimized signature search function (from the second script with improvements)
    local function findSignature(startAddress, signature, direction, maxBytes)
        direction = direction or 1  -- 1 = down, -1 = up
        maxBytes = maxBytes or 500000  -- Optimal search range

        local signatureLength = #signature
        local currentAddress = startAddress
        local bytesChecked = 0
        local endAddress = startAddress + (direction * maxBytes)

        writeLog("Searching for '" .. signature .. "' from 0x" ..
                 string.format("%X", startAddress) .. " direction: " ..
                 (direction == 1 and "down" or "up") .. ", max " .. maxBytes .. " bytes")

        while (direction == 1 and currentAddress < endAddress) or
              (direction == -1 and currentAddress > endAddress) do

            local match = true
            for i = 1, signatureLength do
                local checkAddr
                if direction == 1 then
                    checkAddr = currentAddress + (i - 1)
                else
                    checkAddr = currentAddress - signatureLength + i
                end

                local byteValue = Memory.ReadMemory(checkAddr, 1)
                if byteValue ~= string.byte(signature, i) then
                    match = false
                    break
                end
            end

            if match then
                local resultAddress
                if direction == -1 then
                    resultAddress = currentAddress - signatureLength + 1
                else
                    resultAddress = currentAddress
                end

                writeLog("Found signature at 0x" .. string.format("%X", resultAddress) ..
                         " after checking " .. bytesChecked .. " bytes")
                return resultAddress
            end

            currentAddress = currentAddress + direction
            bytesChecked = bytesChecked + 1
        end

        writeLog("Signature not found in " .. bytesChecked .. " bytes")
        return nil
    end

    -- Find the track array for the long season
    local function findTrackArray()
        local base = Memory.GetModuleBase("F1_2012.exe")
        if not base then
            writeLog("Error: Failed to get base address of F1_2012.exe")
            SCRIPT_RESULT = "Error: Failed to get base address of F1_2012.exe"
            return false
        end

        local ptrAddr = base + 0xDDB23C
        local BASE1 = Memory.ReadMemory(ptrAddr, 4)
        if not BASE1 or BASE1 == 0 then
            SCRIPT_RESULT = "Waiting for game initialization..."
            return false
        end

        -- For long season (isShort = 0)
        local OFFSET1 = Memory.ReadMemory(BASE1 + 0*4 + 0x74, 4)
        originalArray.start = Memory.ReadMemory(OFFSET1 + 0x58, 4)
        originalArray.end_ = Memory.ReadMemory(OFFSET1 + 0x5C, 4)

        if not originalArray.start or not originalArray.end_ then
            SCRIPT_RESULT = "Waiting for race array initialization..."
            return false
        end

        originalArray.size = originalArray.end_ - originalArray.start
        originalArray.count = math.floor(originalArray.size / 8)

        -- Check if we can also find the second array pointer at OFFSET1+630h
        local secondArrayPtr = Memory.ReadMemory(OFFSET1 + 0x630, 4)
		customStructure.address = secondArrayPtr

        if secondArrayPtr and secondArrayPtr ~= 0 then
            writeLog("Found second array pointer at OFFSET1+630h: 0x" ..
                    string.format("%X", secondArrayPtr))
            originalArray.secondPtr = secondArrayPtr

            -- Read and store the first 32 bytes (header) from the second array
            originalArray.header = {}
            for i = 0, 31 do
                originalArray.header[i] = Memory.ReadMemory(secondArrayPtr + i, 1)
            end

            -- Read and store the last 32 bytes (footer) + 4 bytes for FF FF FF FF
            originalArray.footer = {}
            -- First, locate the footer (should be after 20 * 8 bytes of entries)
            local footerStart = secondArrayPtr + 32 + (originalArray.count * 8)
            for i = 0, 35 do  -- 32 bytes footer + 4 bytes for FF FF FF FF
                originalArray.footer[i] = Memory.ReadMemory(footerStart + i, 1)
            end
        else
            writeLog("Warning: Could not find second array pointer at OFFSET1+630h")
        end

        writeLog("Race array found: start=0x" .. string.format("%X", originalArray.start) ..
                ", end=0x" .. string.format("%X", originalArray.end_) ..
                ", size=" .. originalArray.size ..
                " bytes (" .. originalArray.count .. " races)")

        return true
    end

    -- Function to analyze tracks in the database (optimized version)
    local function analyzeTrackDatabase()
        -- Check that the track array was found
        if originalArray.start == 0 then
            return false
        end

        -- Get pointer to the first track
        local firstTrackAddress = Memory.ReadMemory(originalArray.start, 4)
        if not firstTrackAddress or firstTrackAddress == 0 then
            writeLog("Error: Failed to get address of first track")
            return false
        end

        writeLog("First track pointer: 0x" .. string.format("%X", firstTrackAddress))

        -- Find SBDN signature (start of database.bin) by searching UP from track address
        local sbdnAddress = findSignature(firstTrackAddress, "SBDN", -1, 500000)
        if not sbdnAddress then
            writeLog("Error: Failed to find SBDN signature (database.bin)")
            return false
        end

        writeLog("Database.bin found at address: 0x" .. string.format("%X", sbdnAddress))

        -- Find all tracks by ITMS signature (searching DOWN from SBDN)
        local trackCount = 0
        local currentAddress = sbdnAddress
        local maxAddress = sbdnAddress + 5000000  -- Search limit

        while currentAddress and currentAddress < maxAddress do
            currentAddress = findSignature(currentAddress + 4, "ITMS", 1, 500000)

            if currentAddress then
                trackCount = trackCount + 1

                -- Track address is right after ITMS
                local trackAddress = currentAddress + 4

                -- Get track name (at offset +0x20)
                local nameAddress = trackAddress + 0x20
                local trackName = ""
                local emptyTrack = true

                for i = 0, 30 do  -- Name length limit
                    local char = Memory.ReadMemory(nameAddress + i, 1)
                    if char == 0 then break end
                    trackName = trackName .. string.char(char)
                    emptyTrack = false
                end

                -- Stop searching if we find a track with empty name
                if emptyTrack then
                    writeLog("Found track with empty name, stopping search")
                    break
                end

                -- Save track information
                trackDatabase[trackCount] = {
                    address = trackAddress,
                    name = trackName
                }

                writeLog(trackCount .. ". \"" .. trackName .. "\" found at address: 0x" ..
                        string.format("%X", trackAddress))
            end
        end

        writeLog("Total tracks found: " .. trackCount)
        return trackCount > 0
    end

    -- Function to parse the INI file (improved version)
    local function parseCalendarFromIni()
        -- Determine the path to the INI file
        local scriptPath = debug.getinfo(1).source:match("@(.+)$")
        local iniPath = scriptPath:gsub("%.lua$", ".ini")

        -- Check if the file exists
        local iniFile = io.open(iniPath, "r")
        if not iniFile then
            writeLog("Error: Failed to open file " .. iniPath)
            return false
        end

        local inCareerSection = false
        local calendar = {}

        for line in iniFile:lines() do
            -- Remove comments and extra spaces
            line = line:gsub("%;.*$", ""):match("^%s*(.-)%s*$")

            if line:match("^%[career%]") then
                inCareerSection = true
            elseif line:match("^%[") then
                inCareerSection = false
            elseif inCareerSection and line ~= "" then
                -- Format: number. "track name", flag
                local position, trackName, flag = line:match("(%d+)%.%s*\"([^\"]+)\"%s*,%s*(%d+)")

                if not position or not trackName or not flag then
                    -- Try different format without quotes
                    position, trackName, flag = line:match("(%d+)%.%s*([^,]+)%s*,%s*(%d+)")
                end

                if position and trackName and flag then
                    calendar[tonumber(position)] = {
                        name = trackName:match("^%s*(.-)%s*$"),  -- Remove extra spaces
                        flag = tonumber(flag)
                    }
                end
            end
        end

        iniFile:close()

        -- Check that the calendar is not empty
        if next(calendar) == nil then
            writeLog("Error: Calendar in INI file is empty or has incorrect format")
            return false
        end

        customCalendar = calendar

        -- Log the found calendar
        local calendarSize = 0
        local maxPosition = 0
        for position, info in pairs(calendar) do
            calendarSize = calendarSize + 1
            if position > maxPosition then
                maxPosition = position
            end
            writeLog("Calendar [" .. position .. "]: \"" .. info.name .. "\", flag=" .. info.flag)
        end

        writeLog("Total races in calendar: " .. calendarSize .. ", max position: " .. maxPosition)
        return true
    end

    -- Function to create our first custom array (track pointers)
    local function createCustomArray()
        -- Determine how many races are in our calendar
        local calendarSize = 0
        local maxPosition = 0

        for position, _ in pairs(customCalendar) do
            calendarSize = calendarSize + 1
            if position > maxPosition then
                maxPosition = position
            end
        end

        if calendarSize == 0 then
            writeLog("Error: Empty calendar")
            return false
        end

        -- Allocate memory for our array (8 bytes per race)
        -- Use maxPosition to ensure enough space for all entries
        local arraySize = maxPosition * 8
        local arrayAddress = Memory.AllocateMemory(arraySize)

        if not arrayAddress or arrayAddress == 0 then
            writeLog("Error: Failed to allocate memory for custom array")
            return false
        end

        writeLog("Memory allocated for custom array: 0x" ..
                string.format("%X", arrayAddress) .. ", size=" .. arraySize .. " bytes")

        -- Initialize memory to zeros
        for i = 0, arraySize - 1 do
            Memory.WriteMemory(arrayAddress + i, 0, 1)
        end

        -- Fill the array with data
        for position, info in pairs(customCalendar) do
            local entryAddress = arrayAddress + (position - 1) * 8

            -- Find the corresponding track in our database
            local trackAddress = nil
            local bestMatchName = ""
            local bestMatchAddr = 0

            -- First try exact match
            for _, track in pairs(trackDatabase) do
                if track.name:lower() == info.name:lower() then
                    trackAddress = track.address
                    break
                end

                -- Keep track of partial matches
                if track.name:lower():find(info.name:lower(), 1, true) or
                   info.name:lower():find(track.name:lower(), 1, true) then
                    bestMatchName = track.name
                    bestMatchAddr = track.address
                end
            end

            -- If no exact match, try best partial match
            if not trackAddress and bestMatchAddr ~= 0 then
                trackAddress = bestMatchAddr
                writeLog("No exact match for \"" .. info.name ..
                         "\", using partial match \"" .. bestMatchName .. "\"")
            end

            if not trackAddress then
                writeLog("Warning: Track \"" .. info.name .. "\" not found")
                -- Skip this entry
                goto continue
            end

            -- Write track address (4 bytes)
            Memory.WriteMemory(entryAddress, trackAddress, 4)

            -- Write weather flag (1 byte)
            Memory.WriteMemory(entryAddress + 4, info.flag, 1)

            -- Write position number to track
            Memory.WriteMemory(trackAddress, position, 4)
			Memory.WriteMemory(trackAddress + 0x70, position, 4)
			Memory.WriteMemory(trackAddress + 0x110, position, 4)
            writeLog("Wrote position " .. position .. " to track at address 0x" ..
                    string.format("%X", trackAddress))

            writeLog("Added race [" .. position .. "]: \"" .. info.name ..
                    "\" at address 0x" .. string.format("%X", trackAddress) ..
                    ", flag=" .. info.flag)

            ::continue::
        end

        customArray.address = arrayAddress
        customArray.size = arraySize
        customArray.count = maxPosition

        return true
    end

	-- Function to modify second array (objective manager)
	local function createCustomStructure()
		local base = Memory.GetModuleBase("F1_2012.exe")
		if not base then return false end

		local ptrAddr = base + 0xDDB23C
		local BASE1 = Memory.ReadMemory(ptrAddr, 4)
		if not BASE1 or BASE1 == 0 then return false end

		-- For long season (isShort = 0)
		local OFFSET1 = Memory.ReadMemory(BASE1 + 0*4 + 0x74, 4)

		-- Получаем адрес существующей структуры
		local existingStructAddr = Memory.ReadMemory(OFFSET1 + 0x630, 4)
		if not existingStructAddr or existingStructAddr == 0 then
			writeLog("Error: Invalid existing structure address")
			return false
		end

		writeLog("Analyzing existing structure at address: 0x" .. string.format("%X", existingStructAddr))

		-- Ищем второе вхождение FF FF FF FF для определения размера
		local currentPos = existingStructAddr + 32 -- начинаем с начала структуры
		local maxSearchBytes = 1000  -- Максимальный размер поиска
		local foundFirstFFFF = false
		local foundSecondFFFF = false
		local secondFFFFPos = 0

		for i = 0, maxSearchBytes, 1 do
			local b1 = Memory.ReadMemory(currentPos + i, 1)
			local b2 = Memory.ReadMemory(currentPos + i + 1, 1)
			local b3 = Memory.ReadMemory(currentPos + i + 2, 1)
			local b4 = Memory.ReadMemory(currentPos + i + 3, 1)

			if b1 == 0xFF and b2 == 0xFF and b3 == 0xFF and b4 == 0xFF then
					foundFirstFFFF = true
					secondFFFFPos = currentPos + i
					writeLog("Found first FFFF at offset: " .. i)
			end
		end

		if not foundFirstFFFF then
			writeLog("Error: Could not find the first FFFF marker")
			return false
		end

		-- Правильно рассчитываем количество гонок в структуре
		local existingStructSize = (secondFFFFPos - existingStructAddr)
		local totalOffset = 48
		local racesInStruct = math.floor((existingStructSize - totalOffset) / 8)

		writeLog("Existing structure size: " .. existingStructSize .. " bytes, contains " .. racesInStruct .. " races")

		-- Определяем, сколько гонок в нашем календаре
		local calendarSize = 0
		local maxPosition = 0
		for position, _ in pairs(customCalendar) do
			calendarSize = calendarSize + 1
			if position > maxPosition then
				maxPosition = position
			end
		end

		writeLog("Calendar from INI has " .. maxPosition .. " races")

		-- Сохраняем информацию о существующей структуре
		customStructure.address = existingStructAddr
		customStructure.size = existingStructSize

		-- Если размер календаря равен существующему - просто выходим, ничего не делаем
		if maxPosition == racesInStruct then
			writeLog("Calendar size matches existing structure size. No modifications needed.")
			return true
		end

		-- Находим позицию, с которой нужно начать менять данные
		-- Если календарь больше - нужно добавить записи
		-- Если календарь меньше - нужно убрать лишние записи и переместить маркер
		local entriesStart = existingStructAddr + 32  -- 32 header

		if maxPosition < racesInStruct then
			-- Календарь меньше - нужно убрать лишние записи
			writeLog("Calendar is smaller than existing structure. Removing extra entries.")

			-- Вычисляем смещение, где должен быть новый маркер конца
			local newMarkerPos = entriesStart + (maxPosition * 8)

			-- Заполняем 16 байт нулями перед новым маркером конца
			for i = 0, 15 do
				Memory.WriteMemory(newMarkerPos + i, 0, 1)
			end

			-- Устанавливаем новый маркер конца
			Memory.WriteMemory(newMarkerPos + 16, 0xFF, 1)
			Memory.WriteMemory(newMarkerPos + 17, 0xFF, 1)
			Memory.WriteMemory(newMarkerPos + 18, 0xFF, 1)
			Memory.WriteMemory(newMarkerPos + 19, 0xFF, 1)

			writeLog("Removed " .. (racesInStruct - maxPosition) .. " entries. New FF FF FF FF marker at offset " ..
					(newMarkerPos - existingStructAddr + 16))
		else
			-- Календарь больше - добавляем новые записи
			writeLog("Calendar is larger than existing structure. Adding " .. (maxPosition - racesInStruct) .. " entries.")

			-- Заполняем нулями новые записи
			for i = racesInStruct, maxPosition - 1 do
				local entryAddr = entriesStart + (i * 8)
				for j = 0, 7 do
					Memory.WriteMemory(entryAddr + j, 0, 1)
				end
			end

			-- Вычисляем смещение, где должен быть новый маркер конца
			local newMarkerPos = entriesStart + (maxPosition * 8)

			-- Заполняем 16 байт нулями перед новым маркером конца
			for i = 0, 15 do
				Memory.WriteMemory(newMarkerPos + i, 0, 1)
			end

			-- Устанавливаем новый маркер конца
			Memory.WriteMemory(newMarkerPos + 16, 0xFF, 1)
			Memory.WriteMemory(newMarkerPos + 17, 0xFF, 1)
			Memory.WriteMemory(newMarkerPos + 18, 0xFF, 1)
			Memory.WriteMemory(newMarkerPos + 19, 0xFF, 1)

			writeLog("Added " .. (maxPosition - racesInStruct) .. " entries. New FF FF FF FF marker at offset " ..
					(newMarkerPos - existingStructAddr + 16))
		end

		return true
	end

    -- Function to redirect pointers (enhanced version)
    local function redirectPointers()
        local base = Memory.GetModuleBase("F1_2012.exe")
        if not base then return false end

        local ptrAddr = base + 0xDDB23C
        local BASE1 = Memory.ReadMemory(ptrAddr, 4)
        if not BASE1 or BASE1 == 0 then return false end

        -- For long season (isShort = 0)
        local OFFSET1 = Memory.ReadMemory(BASE1 + 0*4 + 0x74, 4)
		local OFFSET2 = Memory.ReadMemory(OFFSET1 + 0x13C, 4)

        -- Save original pointers (for debugging and possible restoration)
        writeLog("Original pointers: start=0x" ..
                string.format("%X", Memory.ReadMemory(OFFSET1 + 0x58, 4)) ..
                ", end=0x" .. string.format("%X", Memory.ReadMemory(OFFSET1 + 0x5C, 4)))

        if originalArray.secondPtr then
            writeLog("Original second array pointer: 0x" ..
                    string.format("%X", Memory.ReadMemory(OFFSET1 + 0x630, 4)))
        end

        -- Redirect first set of pointers to our array
        Memory.WriteMemory(OFFSET1 + 0x58, customArray.address, 4)
        Memory.WriteMemory(OFFSET1 + 0x5C, customArray.address + customArray.size, 4)
		Memory.WriteMemory(OFFSET1 + 0x60, customArray.address + customArray.size, 4)

        writeLog("First pointers redirected to: start=0x" ..
                string.format("%X", customArray.address) ..
                ", end=0x" .. string.format("%X", customArray.address + customArray.size))

        --Update the race count byte
		Memory.WriteMemory(base + 0xC00 + 0x423917, customArray.count, 1)

        writeLog("Updated race count byte to: 0x" ..
               string.format("%X", customArray.count))
        return true
    end

	local function secondModifier()
		--This is very important for proper size checks
		local base = Memory.GetModuleBase("F1_2012.exe")
		local StructSize = customArray.count * 8 + 24
		local StructSize2 = customArray.count * 8 + 28


        Memory.WriteMemory(base + 0xC00 + 0x3E2699, StructSize, 1)
		Memory.WriteMemory(base + 0xC00 + 0x3E26B5, StructSize, 1)
		Memory.WriteMemory(base + 0xC00 + 0x3E26C8, StructSize, 1)
		Memory.WriteMemory(base + 0xC00 + 0x666B9, StructSize2, 1)

		return true
	end

    local initializedByUser = false

    -- Function to initialize the modification
    local function initialize1()
        if initialized then return true end

        writeLog("=========================================")
        writeLog("Starting F1 Calendar Injector initialization")

        -- Step 1: Find the track array
        if not findTrackArray() then
            SCRIPT_RESULT = "Waiting for game initialization..."
            return false
        end

        -- Step 2: Analyze the track database
        if not analyzeTrackDatabase() then
            SCRIPT_RESULT = "Failed to analyze track database"
            return false
        end

        -- Step 3: Parse calendar from INI file
        if not parseCalendarFromIni() then
            SCRIPT_RESULT = "Error parsing INI file"
            return false
        end

        -- Step 4: Create first custom array (track pointers)
        if not createCustomArray() then
            SCRIPT_RESULT = "Failed to create custom array"
            return false
        end

        -- Step 6: Redirect pointers
        if not redirectPointers() then
            SCRIPT_RESULT = "Failed to redirect pointers"
            return false
        end

        writeLog("Initialization completed successfully")
        SCRIPT_RESULT = "Custom calendar successfully activated (" ..
                        customArray.count .. " races). Select your save and hit F6."

        initialized = true
        return true
    end

	local function initialize2()
		if initialized2 then return true end

		-- Step 5: Create second custom array (full structure)
        if not createCustomStructure() then
            SCRIPT_RESULT = "Failed to create custom structure"
            return false
        end

		if not secondModifier() then
            SCRIPT_RESULT = "Failed to modify custom structure"
            return false
        end

		writeLog("Initialization completed successfully")
        SCRIPT_RESULT = "Save modified! Custom calendar successfully launched with (" ..
                        customArray.count .. " races)."

		initialized2 = true
	end

    -- OnFrame function that initializes on F5
    function OnFrame()

        if Keyboard.IsKeyPressed(Keys.VK_F5) then
            if not initialized then

                writeLog("F5 pressed - starting initialization")
                SCRIPT_RESULT = "Initializing calendar by user request (F5)..."
                initialize1()
                initializedByUser = true
            else

                writeLog("F5 pressed - reinitializing")
                SCRIPT_RESULT = "Reinitializing calendar by user request (F5)..."

                initialized = false
                initialize1()
            end
            return true
        end

		--Objective Manager Initialization
        if Keyboard.IsKeyPressed(Keys.VK_F6) then
            if not initialized2 then

                writeLog("F6 pressed - initializating save modifying")
                SCRIPT_RESULT = "Initializing save modifying by user request (F6)..."
                initialize2()
            else

                writeLog("F6 pressed - reinitializing")
                SCRIPT_RESULT = "Reinitializing save modifying by user request (F6)..."

                initialized2 = false
                initialize2()
            end
            return true
        end

        if not initialized and not initializedByUser then
            SCRIPT_RESULT = "Press F5 to activate custom calendar"
        end

        return false
    end

	SCRIPT_RESULT = "Press F5 to activate custom calendar"
end)()
