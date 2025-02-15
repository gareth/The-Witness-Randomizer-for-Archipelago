#include "Input.h"
#include "Memory.h"
#include "Utilities.h"


#define PRINT_INPUT_DEBUG 0
#define KEY_TAP_DURATION 0.4f

InputWatchdog* InputWatchdog::_singleton = nullptr;


void InputWatchdog::initialize() {
	if (_singleton == nullptr) {
		_singleton = new InputWatchdog();
	}
}

InputWatchdog* InputWatchdog::get() {
	return _singleton;
}

InputWatchdog::InputWatchdog() : Watchdog(0.01f) {
	findInteractModeOffset();
	findMenuOpenOffset();
	findCursorRelatedOffsets();
}

void InputWatchdog::action() {
	updateKeyState();
	updateInteractionState();
}

bool InputWatchdog::getButtonState(InputButton key) const {
	// NOTE: The actual value for any given key could be non-zero.
	return currentKeyState[static_cast<int>(key)] != 0;
}

InteractionState InputWatchdog::getInteractionState() const {
	if (currentMenuOpenPercent >= 1.f) {
		return InteractionState::Menu;
	}
	else {
		switch (currentInteractMode) {
		case 0x0:
			return InteractionState::Focusing;
		case 0x1:
			return InteractionState::Solving;
		case 0x2:
			return InteractionState::Walking;
		case 0x3:
			return InteractionState::Cutscene;
		}
	}
}

bool InputWatchdog::consumeInteractionStateChange()
{
	InteractionState currentInteractionState = getInteractionState();
	if (currentInteractionState != previousInteractionState) {
		previousInteractionState == currentInteractionState;
		return true;
	}
	else {
		return false;
	}
}

std::vector<InputButton> InputWatchdog::consumeTapEvents() {
	std::vector<InputButton> output = pendingTapEvents;
	pendingTapEvents.clear();
	return output;
}

std::pair<std::vector<float>, std::vector<float>> InputWatchdog::getMouseRay()
{
	uint64_t mouseFloats = 0;

	_memory->ReadAbsolute(reinterpret_cast<LPVOID>(_memory->GESTURE_MANAGER), &mouseFloats, 0x8);

	mouseFloats += 0x18;

	uint64_t results = reinterpret_cast<uint64_t>(cursorResultsAllocation);


	unsigned char buffer2[] =
		"\x48\xB8\x00\x00\x00\x00\x00\x00\x00\x00" //mov rax [address]
		"\x48\xBA\x00\x00\x00\x00\x00\x00\x00\x00" //mov rdx [address]
		"\x48\xB9\x00\x00\x00\x00\x00\x00\x00\x00" //mov rcx [address]
		"\x48\x83\xEC\x48" // sub rsp,48
		"\xFF\xD0" //call rax
		"\x48\x83\xC4\x48" // add rsp,48
		"\xC3"; //ret

	buffer2[2] = cursorToDirectionFunction & 0xff;
	buffer2[3] = (cursorToDirectionFunction >> 8) & 0xff;
	buffer2[4] = (cursorToDirectionFunction >> 16) & 0xff;
	buffer2[5] = (cursorToDirectionFunction >> 24) & 0xff;
	buffer2[6] = (cursorToDirectionFunction >> 32) & 0xff;
	buffer2[7] = (cursorToDirectionFunction >> 40) & 0xff;
	buffer2[8] = (cursorToDirectionFunction >> 48) & 0xff;
	buffer2[9] = (cursorToDirectionFunction >> 56) & 0xff;
	buffer2[12] = mouseFloats & 0xff;
	buffer2[13] = (mouseFloats >> 8) & 0xff;
	buffer2[14] = (mouseFloats >> 16) & 0xff;
	buffer2[15] = (mouseFloats >> 24) & 0xff;
	buffer2[16] = (mouseFloats >> 32) & 0xff;
	buffer2[17] = (mouseFloats >> 40) & 0xff;
	buffer2[18] = (mouseFloats >> 48) & 0xff;
	buffer2[19] = (mouseFloats >> 56) & 0xff;
	buffer2[22] = results & 0xff;
	buffer2[23] = (results >> 8) & 0xff;
	buffer2[24] = (results >> 16) & 0xff;
	buffer2[25] = (results >> 24) & 0xff;
	buffer2[26] = (results >> 32) & 0xff;
	buffer2[27] = (results >> 40) & 0xff;
	buffer2[28] = (results >> 48) & 0xff;
	buffer2[29] = (results >> 56) & 0xff;

	SIZE_T allocation_size2 = sizeof(buffer2);

	LPVOID allocation_start2 = VirtualAllocEx(_memory->getHandle(), NULL, allocation_size2, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	WriteProcessMemory(_memory->getHandle(), allocation_start2, buffer2, allocation_size2, NULL);
	HANDLE thread2 = CreateRemoteThread(_memory->getHandle(), NULL, 0, (LPTHREAD_START_ROUTINE)allocation_start2, NULL, 0, 0);
	WaitForSingleObject(thread2, INFINITE);

	float resultDirection[3];
	_memory->ReadAbsolute(reinterpret_cast<LPVOID>(results), resultDirection, 0xC);

	std::vector<float> direction(resultDirection, resultDirection + 3);

	std::vector<float> playerPosition = _memory->ReadPlayerPosition(); // Player position and "cursor origin" appear to be the same thing.

	return { playerPosition, direction };
}

void InputWatchdog::updateKeyState() {
	// First, find the address of the key state in memory.
	const std::vector<int> offsets = {
		// Input devices are contained within the Gamelib_Renderer struct. Conveniently enough, Gamelib_Renderer::input_devices is literally the first field
		//   in the struct, so as soon as we dereference the pointer to the renderer we can immediately dereference the input device array.
		Memory::GAMELIB_RENDERER,
		0x0,

		// In the most recent build, the relevant Keyboard struct in input_devices is 0x90 bytes offset from the start of the input_devices array. Note that
		//   this differs from the build with the PDBs due to differences in the input data structures.
		// This value was determined by decompiling InputDevices::get_keyboard_button_state(), which does nothing but retrieve the pointer to the keyboard and
		//   call Keyboard::get_button_state().
		0x90,

		// Finally, the actual input data table is 8 bytes offset from the start of the keyboard struct. This is consistent across builds.
		0x8
	};

	uint64_t keyStateAddress = reinterpret_cast<uint64_t>(_memory->ComputeOffset(offsets));

	// Read the new state into memory.
	int32_t newKeyState[INPUT_KEYSTATE_SIZE];
	if (!_memory->ReadAbsolute(reinterpret_cast<void*>(keyStateAddress), &newKeyState, 0x200 * sizeof(int32_t))) {
		return;
	}

#if PRINT_INPUT_DEBUG
	std::vector<int> changedKeys;
#endif

	for (int keyIndex = 1; keyIndex < INPUT_KEYSTATE_SIZE; keyIndex++) {
		// Test for key changes. Note that we only report values changing to/from zero: the input code sets a value of 3 for the first frame a button is
		//   pressed and 5 for the frame it is released, so if we report an event every time the value changes we'll be adding unnecessary noise.
		//   Additionally, we don't want to explicitly react to 3s and 5s here because we're polling asynchronously, and we may miss the frame that those
		//   values are set.
		bool wasPressed = currentKeyState[keyIndex] != 0;
		bool isPressed = newKeyState[keyIndex] != 0;
		if (!wasPressed && isPressed) {
			// The key was pressed since our last poll. Record when it was pressed.
			pressTimes[static_cast<InputButton>(keyIndex)] = std::chrono::system_clock::now();
		}
		else if (wasPressed && !isPressed) {
			// The key was released since our last poll. Check to see if the press and release are close enough together to count as a tap, and if so,
			//   record it.
			auto result = pressTimes.find(static_cast<InputButton>(keyIndex));
			if (result != pressTimes.end()) {
				std::chrono::duration<float> holdDuration = std::chrono::system_clock::now() - result->second;
				if (holdDuration.count() <= KEY_TAP_DURATION) {
					pendingTapEvents.push_back(static_cast<InputButton>(keyIndex));

#if PRINT_INPUT_DEBUG
					std::wostringstream tapKeyString;
					tapKeyString << "TAPPED: 0x" << std::hex << keyIndex << std::endl;
					OutputDebugStringW(tapKeyString.str().c_str());			
#endif
				}

				pressTimes.erase(result);
			}
		}

#if PRINT_INPUT_DEBUG
		if (wasPressed != isPressed) {
			changedKeys.push_back(keyIndex);
		}
#endif
	}

#if PRINT_INPUT_DEBUG
	if (changedKeys.size() > 0) {
		std::wostringstream activeKeyString;
		activeKeyString << "HELD KEYS:";
		for (int keyIndex : changedKeys) {
			if (newKeyState[keyIndex] != 0) {
				activeKeyString << " 0x" << std::hex << keyIndex;
			}
		}

		activeKeyString << std::endl;

		OutputDebugStringW(activeKeyString.str().c_str());
	}
#endif

	// Save the new key state.
	std::copy(newKeyState, newKeyState + INPUT_KEYSTATE_SIZE, currentKeyState);
}

void InputWatchdog::updateInteractionState() {
	InteractionState oldState = getInteractionState();

	if (interactModeOffset == 0 || !_memory->ReadRelative(reinterpret_cast<void*>(interactModeOffset), &currentInteractMode, sizeof(int32_t))) {
		currentInteractMode = 0x2; // fall back to not solving
	}

	if (menuOpenOffset == 0 || !_memory->ReadRelative(reinterpret_cast<void*>(menuOpenOffset), &currentMenuOpenPercent, sizeof(float))) {
		currentMenuOpenPercent = 0.f; // fall back to not open
	}

#if PRINT_INPUT_DEBUG
	if (getInteractionState() != oldState) {
		std::wostringstream interactionStateString;
		interactionStateString << "INTERACTION STATE: ";
		switch (getInteractionState()) {
		case InteractionState::Walking:
			interactionStateString << "WALKING" << std::endl;
			break;
		case InteractionState::Focusing:
			interactionStateString << "FOCUSING" << std::endl;
			break;
		case InteractionState::Solving:
			interactionStateString << "SOLVING" << std::endl;
			break;
		case InteractionState::Cutscene:
			interactionStateString << "CUTSCENE" << std::endl;
			break;
		case InteractionState::Menu:
			interactionStateString << "MENU" << std::endl;
			break;
		default:
			interactionStateString << "UNKNOWN" << std::endl;
		}

		OutputDebugStringW(interactionStateString.str().c_str());
	}
#endif
}

void InputWatchdog::findInteractModeOffset() {
	// get_cursor_delta_from_mouse_or_gamepad() has a reliable signature to scan for:
	uint64_t cursorDeltaOffset = _memory->executeSigScan({
		0xF3, 0x0F, 0x59, 0xD7,		// MULSS XMM2,XMM7
		0xF3, 0x0F, 0x59, 0xCF,		// MULSS XMM1,XMM7
		0xF3, 0x0F, 0x59, 0xD0,		// MULSS XMM2,XMM0
		0xF3, 0x0F, 0x59, 0xC8,		// MULSS XMM1,XMM0
	});

	if (cursorDeltaOffset == UINT64_MAX) {
		interactModeOffset = 0;
		return;
	}

	// This set of instructions is executed immediately after the call to retrieve the pointer to globals.interact_mode, so we just need to read four bytes prior
	interactModeOffset = 0;
	if (_memory->ReadRelative(reinterpret_cast<void*>(cursorDeltaOffset - 0x4), &interactModeOffset, 0x4)) {
		// Since menu_open_t is a global, any access to it uses an address that's relative to the instruction doing the access, which is conveniently our search offset.
		interactModeOffset += cursorDeltaOffset;
	}
	else {
		interactModeOffset = 0;
	}
}

void InputWatchdog::findMenuOpenOffset() {
	// In order to find menu_open_t, we need to find a usage of it. draw_floating_symbols has a unique entry point:
	uint64_t floatingSymbolOffset = _memory->executeSigScan({
		0x48, 0x63, 0xC3,			// MOVSXD RAX,EBX
		0x48, 0x6B, 0xC8, 0x7C		// IMUL RCX,RAX,0x7C
	});

	if (floatingSymbolOffset == UINT64_MAX) {
		menuOpenOffset = 0;
		return;
	}

	// Skip ahead to the actual read:
	//  0x48, 0x03, 0x0D			// |
	//  0x40, 0x45, 0x43, 0x00		// ADD RCX, qword ptr [floating_symbols.data]
	//  0x0F, 0x2F, 0x79, 0x6C		// COMISS XMM7, dword ptr [RCX + 0x6C]			// floating_symbols.data[N].lifetime_total
	//  0x73, 0x11					// JNC
	//  0xF3, 0x0F, 0x10, 0x0D		// |
	//  ____, ____, ____, ____		// MOVSS XMM1, dword ptr [menu_open_t]
	floatingSymbolOffset += 7 + 17;

	menuOpenOffset = 0;
	if (_memory->ReadRelative(reinterpret_cast<void*>(floatingSymbolOffset), &menuOpenOffset, 0x4)) {
		// Since menu_open_t is a global, any access to it uses an address that's relative to the instruction doing the access.
		menuOpenOffset += floatingSymbolOffset + 0x4;
	}
	else {
		menuOpenOffset = 0;
	}
}

void InputWatchdog::findCursorRelatedOffsets() {
	// cursor_to_direction
	uint64_t offset = _memory->executeSigScan({
		0x40, 0x53,
		0x48, 0x83, 0xEC, 0x60,
		0x48, 0x8B, 0x05,
	});

	cursorToDirectionFunction = _memory->getBaseAddress() + offset;

	cursorResultsAllocation = VirtualAllocEx(_memory->getHandle(), NULL, sizeof(0x20), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	return;
}