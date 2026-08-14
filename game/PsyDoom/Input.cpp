#include "Input.h"

#include "GamepadInput.h"
#include "Config/Config.h"
#include "Doom/Game/p_tick.h"
#include "FatalErrors.h"
#include "ProgArgs.h"
#include "PsxVm.h"
#include "Video.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <SDL.h>

#if defined(__XBOX__)
#include "XboxDiag.h"
#include "XboxLog.h"
// Forward-declare nxdk USB polling function.
extern "C" int usbh_pooling_hubs(void);
#endif

BEGIN_NAMESPACE(Input)

static bool                             gbIsQuitRequested;
static const Uint8*                     gpKeyboardState;
static int                              gNumKeyboardStateKeys;
static std::vector<uint16_t>            gKeyboardKeysPressed;
static std::vector<uint16_t>            gKeyboardKeysJustPressed;
static std::vector<uint16_t>            gKeyboardKeysJustReleased;
static std::vector<char>                gTypedChars;                    // Text input via the OS, respect the keyboard layout and modifiers
static std::vector<MouseButton>         gMouseButtonsPressed;
static std::vector<MouseButton>         gMouseButtonsJustPressed;
static std::vector<MouseButton>         gMouseButtonsJustReleased;
static float                            gGamepadInputs[NUM_GAMEPAD_INPUTS];
static std::vector<GamepadInput>        gGamepadInputsPressed;
static std::vector<GamepadInput>        gGamepadInputsJustPressed;
static std::vector<GamepadInput>        gGamepadInputsJustReleased;
static std::vector<JoystickAxis>        gJoystickAxes;
static std::vector<uint32_t>            gJoystickAxesPressed;
static std::vector<uint32_t>            gJoystickAxesJustPressed;
static std::vector<uint32_t>            gJoystickAxesJustReleased;
static std::vector<uint32_t>            gJoystickButtonsPressed;
static std::vector<uint32_t>            gJoystickButtonsJustPressed;
static std::vector<uint32_t>            gJoystickButtonsJustReleased;
static std::vector<JoyHat>              gJoystickHatsPressed;
static std::vector<JoyHat>              gJoystickHatsJustPressed;
static std::vector<JoyHat>              gJoystickHatsJustReleased;

static SDL_GameController*  gpGameController;

// Player two's pad, for splitscreen.
//
// The controller above is the one the whole event driven input path is built around, and it stays exactly as it is -
// single player must not be disturbed by any of this. Player two is read straight from SDL instead of through those
// events, which avoids threading a controller index through the event handling for a second player who only ever
// needs the current state of a stick or a button.
static SDL_GameController*  gpGameController2;
static SDL_JoystickID       gJoystickId2 = -1;

#if defined(__XBOX__)
    // Event tallies for player two, kept because sampling once a second cannot see a stick that was pushed between
    // samples. Every 'p2 gap' reading of zero so far was taken instantaneously, so it proves nothing on its own: a
    // pad held for half a second between two samples looks identical to a pad that is not there. These count events
    // as they arrive and hold the largest deflection seen, so any input at all during a window shows up in it.
    uint32_t    gXbP2JoyAxisEvents = 0;      // SDL_JOYAXISMOTION matching player two
    uint32_t    gXbP2CtrlAxisEvents = 0;     // SDL_CONTROLLERAXISMOTION matching player two
    uint32_t    gXbP2ButtonEvents = 0;       // Buttons, which are known to work
    uint32_t    gXbP2UnmatchedAxis = 0;      // Axis events from a device that is neither player - the id mismatch case
    int32_t     gXbP2PeakAxis[6] = {};       // Largest absolute raw value seen per axis
#endif

// Player two's pad, sampled once per update.
//
// Polling gives the current state but not the edges, and 'just pressed' is what pause, menus and weapon switching are
// all built on. Keeping this frame's values and last frame's gives the edges without touching the event path that
// player one depends on.
static float gGamepadInputsP2[NUM_GAMEPAD_INPUTS];
static float gGamepadInputsP2Old[NUM_GAMEPAD_INPUTS];
static float getGamepadInputValueP2Raw(const GamepadInput input) noexcept;
static SDL_Joystick*        gpJoystick;         // Note: if there is a game controller then this joystick will be managed by that and not closed manually by this module!
static SDL_JoystickID       gJoystickId;

// Mouse movements this frame
static float gMouseMovementX;
static float gMouseMovementY;
static float gMouseWheelAxisMovements[NUM_MOUSE_WHEEL_AXES];

// Did the window just lose focus?
static bool gbWindowFocusJustLost;

//------------------------------------------------------------------------------------------------------------------------------------------
// Vector utility functions
//------------------------------------------------------------------------------------------------------------------------------------------
template <class T>
static inline bool vectorContainsValue(const std::vector<T>& vec, const T val) noexcept {
    const auto endIter = vec.end();
    const auto iter = std::find(vec.begin(), endIter, val);
    return (iter != endIter);
}

template <class T>
static inline void removeValueFromVector(const T val, std::vector<T>& vec) noexcept {
    auto endIter = vec.end();
    auto iter = std::find(vec.begin(), endIter, val);

    while (iter != endIter) {
        iter = vec.erase(iter);
        endIter = vec.end();
        iter = std::find(iter, endIter, val);
    }
}

template <class T>
static inline void emptyAndShrinkVector(std::vector<T>& vec) noexcept {
    vec.clear();
    vec.shrink_to_fit();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Update the value of a joystick axis in the vector of values: removes the value if it has reached '0'
//------------------------------------------------------------------------------------------------------------------------------------------
static void updateJoystickAxisValue(const uint32_t axis, const float value) noexcept {
    // Search for the existing value of this axis: will need to remove or update it if found
    auto iter = std::find_if(gJoystickAxes.begin(), gJoystickAxes.end(), [=](const JoystickAxis& axisValue) noexcept { return (axisValue.axis == axis); });

    if (value == 0.0f) {
        if (iter != gJoystickAxes.end()) {
            gJoystickAxes.erase(iter);
        }
    }
    else {
        if (iter != gJoystickAxes.end()) {
            iter->value = value;
        } else {
            gJoystickAxes.emplace_back(JoystickAxis{ axis, value});
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Convert an SDL axis value to a -1 to + 1 range float.
//------------------------------------------------------------------------------------------------------------------------------------------
static float sdlAxisValueToFloat(const int16_t axis) noexcept {
    if (axis >= 0) {
        return (float) axis / 32767.0f;
    } else {
        return (float) axis / 32768.0f;
    }
}

#if defined(__XBOX__)
// Apply a scaled dead-zone to raw stick axis values so hardware drift doesn't
// produce spurious input. Values within ±DEADZONE map to zero; values outside
// are re-scaled to use the full [-32767..32767] range.
static int16_t applyStickDeadzone(const int16_t v) noexcept {
    // Wide enough to cover this pad's centre error.
    //
    // The right stick drifts the view even at rest, and a deadzone applied later in the turning code did not stop it -
    // because turning is not the only path it takes. An axis past 'AnalogToDigitalThreshold' also counts as the turn
    // button being held, and that path had no deadzone of its own. Doing it here, at the source, covers both: the
    // reading is zero before anything downstream ever sees it.
    //
    // Measured drift on this pad reached about a tenth of full deflection. 8000 is a quarter, which covers it with
    // room to spare while still leaving three quarters of the stick's travel usable.
    constexpr int16_t DEADZONE = 8000;
    if (v > -DEADZONE && v < DEADZONE) return 0;
    const int sign   = (v > 0) ? 1 : -1;
    const int absV   = (v > 0) ? (int)v : -(int)v;
    const int scaled = ((absV - DEADZONE) * 32767) / (32767 - DEADZONE);
    return (int16_t)(sign * (scaled < 32767 ? scaled : 32767));
}
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Close the currently open game controller or generic joystick (if any).
// Also clears up any related inputs.
//------------------------------------------------------------------------------------------------------------------------------------------
static void closeCurrentGameController() noexcept {
    std::memset(gGamepadInputs, 0, sizeof(gGamepadInputs));
    gGamepadInputsPressed.clear();
    gGamepadInputsJustPressed.clear();
    gGamepadInputsJustReleased.clear();

    gJoystickAxes.clear();
    gJoystickAxesPressed.clear();
    gJoystickAxesJustPressed.clear();
    gJoystickAxesJustReleased.clear();

    gJoystickButtonsPressed.clear();
    gJoystickButtonsJustPressed.clear();
    gJoystickButtonsJustReleased.clear();

    gJoystickHatsPressed.clear();
    gJoystickHatsJustPressed.clear();
    gJoystickHatsJustReleased.clear();

    // Close the current game controller, if there is any.
    // Note that closing a game controller closes the associated joystick automatically also.
    if (gpGameController) {
        SDL_GameControllerClose(gpGameController);
        gpGameController = nullptr;
        gpJoystick = nullptr;       // Managed by the game controller object, already closed!
    }

    // Close the current generic joystick, if that's all we have and not the 'game controller' interface
    if (gpJoystick) {
        SDL_JoystickClose(gpJoystick);
        gpJoystick = nullptr;
    }

    gJoystickId = {};
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Rescans for SDL game controllers and generic joysticks to use: just uses the first available controller or joystick.
// This may choose wrong in a multi-gamepad/joystick situation but the user can always disconnect one to clarify which one is wanted.
// Most computer users would probably only want one gamepad or joystick connected at a time anyway?
//------------------------------------------------------------------------------------------------------------------------------------------
static void rescanGameControllers() noexcept {
    // If we already have a gamepad or generic joystick then just re-check that it is still connected.
    // Note that we can check if a gamepad is connected by checking if the associated joystick is connected.
    if (gpJoystick) {
        if (!SDL_JoystickGetAttached(gpJoystick)) {
            closeCurrentGameController();
        }
    }

    // See if there are any joysticks connected.
    // Note: a return of < 0 means an error, which we will ignore:
    const int numJoysticks = SDL_NumJoysticks();

    for (int joyIdx = 0; joyIdx < numJoysticks; ++joyIdx) {
        // If we find a valid game controller or generic joystick then try to open it.
        // If we succeed then our work is done!
        if (SDL_IsGameController(joyIdx)) {
            // This is a game controller - try opening that way
            // Player one takes the first controller found, player two the next one
            if (!gpGameController) {
                gpGameController = SDL_GameControllerOpen(joyIdx);

                if (gpGameController) {
                    gpJoystick = SDL_GameControllerGetJoystick(gpGameController);
                    gJoystickId = SDL_JoystickInstanceID(gpJoystick);
                    continue;   // Keep looking, so a second pad can be picked up for splitscreen
                }
            }
            else if (!gpGameController2) {
                gpGameController2 = SDL_GameControllerOpen(joyIdx);

                if (gpGameController2) {
                    gJoystickId2 = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gpGameController2));
                    break;
                }
            }
        }

        // Fallback to opening the controller as a generic joystick if it's not supported through the game controller interface.
        //
        // Only ever for player one. Looking for a second pad means this loop no longer stops at the first device, and
        // without this guard a second device would overwrite 'gJoystickId' - which is what player one's event handling
        // matches every incoming event against, so player one would stop receiving input entirely.
        if (gpJoystick)
            continue;

        gpJoystick = SDL_JoystickOpen(joyIdx);

        if (gpJoystick) {
            gJoystickId = SDL_JoystickInstanceID(gpJoystick);
            continue;
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Update the status for the specified joystick hat and generate events if required
//------------------------------------------------------------------------------------------------------------------------------------------
static void updateJoystickHat(const JoyHat hat, const bool bPressed) noexcept {
    if (bPressed) {
        // Just pressed?
        if (!vectorContainsValue(gJoystickHatsPressed, hat)) {
            removeValueFromVector(hat, gJoystickHatsJustReleased);
            gJoystickHatsPressed.push_back(hat);
            gJoystickHatsJustPressed.push_back(hat);
        }
    }
    else {
        // Just released?
        if (vectorContainsValue(gJoystickHatsPressed, hat)) {
            gJoystickHatsJustReleased.push_back(hat);
            removeValueFromVector(hat, gJoystickHatsPressed);
            removeValueFromVector(hat, gJoystickHatsJustPressed);
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Handle events sent by SDL (keypresses and such)
//------------------------------------------------------------------------------------------------------------------------------------------
static void handleSdlEvents() noexcept {
    SDL_Event sdlEvent;
    bool bConsumeEvents = false;

    while (SDL_PollEvent(&sdlEvent) != 0) {
        switch (sdlEvent.type) {
            case SDL_QUIT:
                // The application is requesting to quit
                gbIsQuitRequested = true;
                break;

            case SDL_WINDOWEVENT: {
                switch (sdlEvent.window.event) {
                    case SDL_WINDOWEVENT_FOCUS_GAINED: {
                        // Note: don't grab mouse input here, wait until the window's client area is actually clicked.
                        // This makes resizing and such easier in windowed mode.
                        bConsumeEvents = true;

                        // Tell the game to ignore firing until the fire button is released.
                        // This prevents clicking on the window with a mouse for example triggering firing.
                        gbIgnoreCurrentAttack = true;
                    }   break;

                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        SDL_ShowCursor(SDL_ENABLE);
                        SDL_SetWindowGrab(Video::gpSdlWindow, SDL_FALSE);
                        SDL_SetRelativeMouseMode(SDL_FALSE);
                        gbWindowFocusJustLost = true;
                        break;
                }
            }   break;

            case SDL_TEXTINPUT: {
                // Typed text which respects the current keyboard layout and modifiers like the shift key
                for (const char typedChar : sdlEvent.text.text) {
                    if (typedChar) {
                        gTypedChars.push_back(typedChar);
                    } else {
                        break;
                    }
                }
            }   break;

            case SDL_KEYDOWN: {
                const uint16_t scancode = (uint16_t) sdlEvent.key.keysym.scancode;

                // Note: ignore artifically repeated key events
                if ((!sdlEvent.key.repeat) && (scancode < NUM_KEYBOARD_KEYS)) {
                    removeValueFromVector(scancode, gKeyboardKeysJustReleased);
                    gKeyboardKeysPressed.push_back(scancode);
                    gKeyboardKeysJustPressed.push_back(scancode);
                }
            }   break;

            case SDL_KEYUP: {
                const uint16_t scancode = (uint16_t) sdlEvent.key.keysym.scancode;

                // Note: ignore artifically repeated key events
                if ((!sdlEvent.key.repeat) && (scancode < NUM_KEYBOARD_KEYS)) {
                    removeValueFromVector(scancode, gKeyboardKeysPressed);
                    removeValueFromVector(scancode, gKeyboardKeysJustPressed);
                    gKeyboardKeysJustReleased.push_back(scancode);
                }
            }   break;

            case SDL_MOUSEBUTTONDOWN: {
                // Capture the mouse on a click if we haven't captured it yet
                if (!SDL_GetRelativeMouseMode()) {
                    SDL_ShowCursor(SDL_DISABLE);
                    SDL_SetWindowGrab(Video::gpSdlWindow, SDL_TRUE);
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                    bConsumeEvents = true;

                    // Tell the game to ignore firing until the fire button is released.
                    // This prevents clicking on the window with a mouse for example triggering firing.
                    gbIgnoreCurrentAttack = true;
                }

                // Handle the button click
                const MouseButton button = (MouseButton)(sdlEvent.button.button - 1);

                if ((uint8_t) button < NUM_MOUSE_BUTTONS) {
                    removeValueFromVector(button, gMouseButtonsJustReleased);
                    gMouseButtonsPressed.push_back(button);
                    gMouseButtonsJustPressed.push_back(button);
                }
            } break;

            case SDL_MOUSEBUTTONUP: {
                const MouseButton button = (MouseButton)(sdlEvent.button.button - 1);

                if ((uint8_t) button < NUM_MOUSE_BUTTONS) {
                    removeValueFromVector(button, gMouseButtonsPressed);
                    removeValueFromVector(button, gMouseButtonsJustPressed);
                    gMouseButtonsJustReleased.push_back(button);
                }
            } break;

            case SDL_MOUSEMOTION: {
                // Only register movement if we have captured the mouse
                if (SDL_GetRelativeMouseMode()) {
                    gMouseMovementX += (float) sdlEvent.motion.xrel;
                    gMouseMovementY += (float) sdlEvent.motion.yrel;
                } else {
                    gMouseMovementX = 0.0f;
                    gMouseMovementY = 0.0f;
                }
            } break;

            case SDL_MOUSEWHEEL: {
                // Only register movement if we have captured the mouse
                if (SDL_GetRelativeMouseMode()) {
                    gMouseWheelAxisMovements[0] += (float) sdlEvent.wheel.x;
                    gMouseWheelAxisMovements[1] += (float) sdlEvent.wheel.y;
                } else {
                    gMouseWheelAxisMovements[0] = 0.0f;
                    gMouseWheelAxisMovements[1] = 0.0f;
                }
            } break;

            case SDL_CONTROLLERAXISMOTION: {
                // Player two's axes, from the event rather than by polling.
                //
                // 'SDL_GameControllerGetAxis' does not work on this platform: it was measured returning the same
                // values for eight seconds while both pads were being used, and 'SDL_GameControllerUpdate' did not
                // change that. Player one has always worked because player one is fed from these events and never
                // asks for the current value. Player two now does the same.
                if ((gJoystickId2 >= 0) && (sdlEvent.caxis.which == gJoystickId2)) {
                    Sint16 caxisRawP2 = sdlEvent.caxis.value;

                    #if defined(__XBOX__)
                        gXbP2CtrlAxisEvents++;
                    #endif

                    if (sdlEvent.caxis.axis <= SDL_CONTROLLER_AXIS_RIGHTY) {
                        caxisRawP2 = applyStickDeadzone(caxisRawP2);
                    }

                    const GamepadInput inputP2 = GamepadInputUtils::sdlAxisToInput(sdlEvent.caxis.axis);

                    if (inputP2 != GamepadInput::INVALID) {
                        gGamepadInputsP2[(uint8_t) inputP2] = sdlAxisValueToFloat(caxisRawP2);
                    }

                    break;
                }

                if (sdlEvent.cbutton.which == gJoystickId) {
                    Sint16 caxisRaw = sdlEvent.caxis.value;
#if defined(__XBOX__)
                    // Apply deadzone to stick axes (LEFTX=0,LEFTY=1,RIGHTX=2,RIGHTY=3)
                    if (sdlEvent.caxis.axis <= SDL_CONTROLLER_AXIS_RIGHTY) {
                        caxisRaw = applyStickDeadzone(caxisRaw);
                    }
#endif
                    const GamepadInput input = GamepadInputUtils::sdlAxisToInput(sdlEvent.caxis.axis);

                    if (input != GamepadInput::INVALID) {
                        const float pressedThreshold = Config::gAnalogToDigitalThreshold;
                        const uint8_t inputIdx = (uint8_t) input;

                        // See if there is a change in the 'pressed' status
                        const bool bPrevPressed = (std::abs(gGamepadInputs[inputIdx]) >= pressedThreshold);
                        const float inputF = sdlAxisValueToFloat(caxisRaw);
                        const float inputFAbs = std::abs(inputF);
                        const bool bNowPressed = (inputFAbs >= pressedThreshold);

                        // Update input value
                        gGamepadInputs[inputIdx] = inputF;

                        // Generate events for the analog input
                        if (bPrevPressed != bNowPressed) {
                            if (bNowPressed) {
                                removeValueFromVector(input, gGamepadInputsJustReleased);
                                gGamepadInputsPressed.push_back(input);
                                gGamepadInputsJustPressed.push_back(input);
                            } else {
                                removeValueFromVector(input, gGamepadInputsPressed);
                                removeValueFromVector(input, gGamepadInputsJustPressed);
                                gGamepadInputsJustReleased.push_back(input);
                            }
                        }
                    }
                }
            }   break;

            case SDL_JOYAXISMOTION: {
                #if defined(__XBOX__)
                    // An axis event that matches neither player means the ids we are testing against are wrong,
                    // which is a different fault from the events not arriving at all. Counted so the two can be told apart.
                    if ((sdlEvent.jaxis.which != gJoystickId) && (sdlEvent.jaxis.which != gJoystickId2)) {
                        gXbP2UnmatchedAxis++;
                    }
                #endif

                // Player two's sticks and triggers.
                //
                // This is the event that carries them, and the reason player two could press buttons but never move.
                // Their axes were built on 'SDL_CONTROLLERAXISMOTION', which this platform does not deliver - player
                // one has always been fed from the joystick event below, and never needed the controller one. Buttons
                // worked because 'SDL_CONTROLLERBUTTONDOWN' really does arrive, which made the fault look like a
                // routing problem when player two was simply listening to the wrong event.
                //
                // Measured: 'p2 gap: pad ly=0 lx=0 | controls attack=1000' - buttons through, axes never stored.
                #if defined(__XBOX__)
                    if ((gJoystickId2 >= 0) && (sdlEvent.jaxis.which == gJoystickId2)) {
                        const uint32_t axis = sdlEvent.jaxis.axis;
                        Sint16 axisRawP2 = sdlEvent.jaxis.value;

                        gXbP2JoyAxisEvents++;

                        if (axis < 6) {
                            const int32_t absRaw = (sdlEvent.jaxis.value < 0) ? -(int32_t) sdlEvent.jaxis.value : sdlEvent.jaxis.value;

                            if (absRaw > gXbP2PeakAxis[axis]) {
                                gXbP2PeakAxis[axis] = absRaw;
                            }
                        }

                        // Sticks are axes 0, 1, 3 and 4; the triggers on 2 and 5 are left unscaled, as for player one
                        if ((axis == 0) || (axis == 1) || (axis == 3) || (axis == 4)) {
                            axisRawP2 = applyStickDeadzone(axisRawP2);
                        }

                        // Raw joystick axis order to the game controller axes the bindings are written against
                        SDL_GameControllerAxis ctrlAxis = SDL_CONTROLLER_AXIS_INVALID;

                        switch (axis) {
                            case 0:     ctrlAxis = SDL_CONTROLLER_AXIS_LEFTX;            break;
                            case 1:     ctrlAxis = SDL_CONTROLLER_AXIS_LEFTY;            break;
                            case 2:     ctrlAxis = SDL_CONTROLLER_AXIS_TRIGGERLEFT;      break;
                            case 3:     ctrlAxis = SDL_CONTROLLER_AXIS_RIGHTX;           break;
                            case 4:     ctrlAxis = SDL_CONTROLLER_AXIS_RIGHTY;           break;
                            case 5:     ctrlAxis = SDL_CONTROLLER_AXIS_TRIGGERRIGHT;     break;
                            default:    break;
                        }

                        if (ctrlAxis != SDL_CONTROLLER_AXIS_INVALID) {
                            const GamepadInput inputP2 = GamepadInputUtils::sdlAxisToInput((uint8_t) ctrlAxis);

                            if (inputP2 != GamepadInput::INVALID) {
                                gGamepadInputsP2[(uint8_t) inputP2] = sdlAxisValueToFloat(axisRawP2);
                            }
                        }

                        break;
                    }
                #endif

                if (sdlEvent.jaxis.which == gJoystickId) {
                    const uint32_t axis = sdlEvent.jaxis.axis;
                    Sint16 axisRaw = sdlEvent.jaxis.value;
#if defined(__XBOX__)
                    // Apply scaled dead-zone to analog sticks (axes 0,1,3,4).
                    // Triggers (axes 2,5) are left unscaled.
                    if (axis == 0 || axis == 1 || axis == 3 || axis == 4) {
                        axisRaw = applyStickDeadzone(axisRaw);
                    }
#endif
                    // See if there is a change in the 'pressed' status
                    const float pressedThreshold = Config::gAnalogToDigitalThreshold;

                    const bool bPrevPressed = (std::abs(Input::getJoystickAxisValue(axis)) >= pressedThreshold);
                    const float inputF = sdlAxisValueToFloat(axisRaw);
                    const float inputFAbs = std::abs(inputF);
                    const bool bNowPressed = (inputFAbs >= pressedThreshold);

                    // Update input value
                    updateJoystickAxisValue(axis, inputF);

                    // Generate events for the analog input
                    if (bPrevPressed != bNowPressed) {
                        if (bNowPressed) {
                            removeValueFromVector(axis, gJoystickAxesJustReleased);
                            gJoystickAxesPressed.push_back(axis);
                            gJoystickAxesJustPressed.push_back(axis);
                        } else {
                            removeValueFromVector(axis, gJoystickAxesPressed);
                            removeValueFromVector(axis, gJoystickAxesJustPressed);
                            gJoystickAxesJustReleased.push_back(axis);
                        }
                    }
                }
            }   break;

            case SDL_CONTROLLERBUTTONDOWN: {
                if ((gJoystickId2 >= 0) && (sdlEvent.cbutton.which == gJoystickId2)) {
                    #if defined(__XBOX__)
                        gXbP2ButtonEvents++;
                    #endif

                    const GamepadInput inputP2 = GamepadInputUtils::sdlButtonToInput(sdlEvent.cbutton.button);

                    if (inputP2 != GamepadInput::INVALID) {
                        gGamepadInputsP2[(uint8_t) inputP2] = 1.0f;
                    }

                    break;
                }

                if (sdlEvent.cbutton.which == gJoystickId) {
                    const GamepadInput input = GamepadInputUtils::sdlButtonToInput(sdlEvent.cbutton.button);

                    if (input != GamepadInput::INVALID) {
                        removeValueFromVector(input, gGamepadInputsJustReleased);
                        gGamepadInputsPressed.push_back(input);
                        gGamepadInputsJustPressed.push_back(input);
                        gGamepadInputs[(uint8_t) input] = 1.0f;
                    }
                }
            }   break;

            case SDL_JOYBUTTONDOWN: {
                if (sdlEvent.jbutton.which == gJoystickId) {
                    const uint32_t button = sdlEvent.jbutton.button;
                    removeValueFromVector(button, gJoystickButtonsJustReleased);
                    gJoystickButtonsPressed.push_back(button);
                    gJoystickButtonsJustPressed.push_back(button);
                }
            }   break;

            case SDL_CONTROLLERBUTTONUP: {
                if ((gJoystickId2 >= 0) && (sdlEvent.cbutton.which == gJoystickId2)) {
                    const GamepadInput inputP2 = GamepadInputUtils::sdlButtonToInput(sdlEvent.cbutton.button);

                    if (inputP2 != GamepadInput::INVALID) {
                        gGamepadInputsP2[(uint8_t) inputP2] = 0.0f;
                    }

                    break;
                }

                if (sdlEvent.cbutton.which == gJoystickId) {
                    const GamepadInput input = GamepadInputUtils::sdlButtonToInput(sdlEvent.cbutton.button);

                    if (input != GamepadInput::INVALID) {
                        gGamepadInputsJustReleased.push_back(input);
                        removeValueFromVector(input, gGamepadInputsPressed);
                        removeValueFromVector(input, gGamepadInputsJustPressed);
                        gGamepadInputs[(uint8_t) input] = 0.0f;
                    }
                }
            }   break;

            case SDL_JOYBUTTONUP: {
                if (sdlEvent.jbutton.which == gJoystickId) {
                    const uint32_t button = sdlEvent.jbutton.button;
                    gJoystickButtonsJustReleased.push_back(button);
                    removeValueFromVector(button, gJoystickButtonsPressed);
                    removeValueFromVector(button, gJoystickButtonsJustPressed);
                }
            }   break;

            case SDL_JOYHATMOTION: {
                if (sdlEvent.jhat.which == gJoystickId) {
                    const uint8_t hatNum = sdlEvent.jhat.hat;
                    updateJoystickHat(JoyHat(JoyHatDir::Up, hatNum), (sdlEvent.jhat.value & SDL_HAT_UP));
                    updateJoystickHat(JoyHat(JoyHatDir::Down, hatNum), (sdlEvent.jhat.value & SDL_HAT_DOWN));
                    updateJoystickHat(JoyHat(JoyHatDir::Left, hatNum), (sdlEvent.jhat.value & SDL_HAT_LEFT));
                    updateJoystickHat(JoyHat(JoyHatDir::Right, hatNum), (sdlEvent.jhat.value & SDL_HAT_RIGHT));
                }
            }   break;

            case SDL_JOYDEVICEADDED:
            case SDL_JOYDEVICEREMOVED:
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
            case SDL_CONTROLLERDEVICEREMAPPED:
                rescanGameControllers();
                break;
        }
    }

    if (bConsumeEvents) {
        consumeEvents();
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Initialize input handling
//------------------------------------------------------------------------------------------------------------------------------------------
void init() noexcept {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        FatalErrors::raise("Failed to initialize the SDL joystick input subsystem!");
    }

    SDL_GameControllerEventState(SDL_ENABLE);       // Want game controller events

    gbIsQuitRequested = false;

    gpKeyboardState = SDL_GetKeyboardState(&gNumKeyboardStateKeys);
    gKeyboardKeysJustPressed.reserve(32);
    gKeyboardKeysJustReleased.reserve(32);
    gTypedChars.reserve(32);

    gMouseButtonsPressed.reserve(NUM_MOUSE_BUTTONS);
    gMouseButtonsJustPressed.reserve(NUM_MOUSE_BUTTONS);
    gMouseButtonsJustReleased.reserve(NUM_MOUSE_BUTTONS);

    gGamepadInputsPressed.reserve(NUM_GAMEPAD_INPUTS);
    gGamepadInputsJustPressed.reserve(NUM_GAMEPAD_INPUTS);
    gGamepadInputsJustReleased.reserve(NUM_GAMEPAD_INPUTS);

    gJoystickAxes.reserve(16);
    gJoystickAxesPressed.reserve(16);
    gJoystickAxesJustPressed.reserve(16);
    gJoystickAxesJustReleased.reserve(16);

    gJoystickButtonsPressed.reserve(32);
    gJoystickButtonsJustPressed.reserve(32);
    gJoystickButtonsJustReleased.reserve(32);

    gJoystickHatsPressed.reserve(32);
    gJoystickHatsJustPressed.reserve(32);
    gJoystickHatsJustReleased.reserve(32);

    gMouseMovementX = 0.0f;
    gMouseMovementY = 0.0f;

    gbWindowFocusJustLost = false;

    rescanGameControllers();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Shutdown input handling
//------------------------------------------------------------------------------------------------------------------------------------------
void shutdown() noexcept {
    consumeEvents();
    closeCurrentGameController();

    gbWindowFocusJustLost = false;

    gMouseMovementX = 0.0f;
    gMouseMovementY = 0.0f;

    emptyAndShrinkVector(gJoystickHatsJustReleased);
    emptyAndShrinkVector(gJoystickHatsJustPressed);
    emptyAndShrinkVector(gJoystickHatsPressed);

    emptyAndShrinkVector(gJoystickButtonsJustReleased);
    emptyAndShrinkVector(gJoystickButtonsJustPressed);
    emptyAndShrinkVector(gJoystickButtonsPressed);

    emptyAndShrinkVector(gJoystickAxesJustReleased);
    emptyAndShrinkVector(gJoystickAxesJustPressed);
    emptyAndShrinkVector(gJoystickAxesPressed);
    emptyAndShrinkVector(gJoystickAxes);

    emptyAndShrinkVector(gGamepadInputsJustReleased);
    emptyAndShrinkVector(gGamepadInputsJustPressed);
    emptyAndShrinkVector(gGamepadInputsPressed);

    emptyAndShrinkVector(gMouseButtonsJustReleased);
    emptyAndShrinkVector(gMouseButtonsJustPressed);
    emptyAndShrinkVector(gMouseButtonsPressed);

    emptyAndShrinkVector(gTypedChars);
    emptyAndShrinkVector(gKeyboardKeysJustReleased);
    emptyAndShrinkVector(gKeyboardKeysJustPressed);
    emptyAndShrinkVector(gKeyboardKeysPressed);

    gpKeyboardState = nullptr;
    gbIsQuitRequested = false;

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Generates input events like key down; should be called once per frame
//------------------------------------------------------------------------------------------------------------------------------------------
void update() noexcept {
    #if defined(__XBOX__)
        const unsigned long long inputStart = XboxLog::nowMicros();
    #endif

    if (!ProgArgs::gbHeadlessMode) {
#if defined(__XBOX__)
        // On nxdk, usbh_pooling_hubs() must be called regularly from the application
        // to keep the USB stack alive and process hub events (connect/disconnect, IRQ re-arm).
        usbh_pooling_hubs();
        XboxDiag::tickUsb();  // diagnostic: count USB service calls
#endif
#if defined(__XBOX__)
        // Remember player two's pad as it was BEFORE this frame's events are read.
        //
        // This has to happen first. It used to run after 'handleSdlEvents', which copied the values those events had
        // just written - so the previous frame and the current one were always identical and no edge was ever seen.
        // That is why player two could not pause or work a menu while weapon switching worked fine: weapon switching
        // takes its edge from 'gOldTickInputs', whereas pause and every menu action go through
        // 'Controls::isJustPressed', which compares these two arrays.
        if (gpGameController2) {
            std::memcpy(gGamepadInputsP2Old, gGamepadInputsP2, sizeof(gGamepadInputsP2));
        }
#endif

        handleSdlEvents();

#if defined(__XBOX__)
        // Player two's inputs for this frame, now with a usable previous frame beside them
        if (gpGameController2) {
            // Player two's values come from the events above, not from polling.
            //
            // They used to be sampled here with 'SDL_GameControllerGetAxis', which does not work on this platform -
            // measured returning identical values for eight seconds while both pads were in use, unaffected by
            // 'SDL_GameControllerUpdate'. Sampling here would overwrite the good event driven values with those frozen
            // ones every frame. The previous frame is remembered above instead, before the events are read.

            // Report what player two's inputs actually are, so the fix is verifiable rather than assumed
            {
                static uint32_t sLastAxisReport = 0;
                const uint32_t nowMs = SDL_GetTicks();

                if (nowMs - sLastAxisReport >= 1000u) {
                    sLastAxisReport = nowMs;

                    // Cumulative, so a stick pushed between two samples still shows up
                    XBOX_LOGI(
                        Input,
                        "p2 events: joyaxis=%u ctrlaxis=%u btn=%u unmatched=%u | peak lx=%d ly=%d rx=%d ry=%d | ids p1=%d p2=%d",
                        gXbP2JoyAxisEvents, gXbP2CtrlAxisEvents, gXbP2ButtonEvents, gXbP2UnmatchedAxis,
                        gXbP2PeakAxis[0], gXbP2PeakAxis[1], gXbP2PeakAxis[3], gXbP2PeakAxis[4],
                        (int) gJoystickId, (int) gJoystickId2
                    );

                    XBOX_LOGI(
                        Input,
                        "p2 pad (from events): lx=%d ly=%d rx=%d attack=%d start=%d (x1000)",
                        (int)(gGamepadInputsP2[(uint8_t) GamepadInput::AXIS_LEFT_X] * 1000.0f),
                        (int)(gGamepadInputsP2[(uint8_t) GamepadInput::AXIS_LEFT_Y] * 1000.0f),
                        (int)(gGamepadInputsP2[(uint8_t) GamepadInput::AXIS_RIGHT_X] * 1000.0f),
                        (int)(gGamepadInputsP2[(uint8_t) GamepadInput::AXIS_TRIG_RIGHT] * 1000.0f),
                        (int)(gGamepadInputsP2[(uint8_t) GamepadInput::BTN_START] * 1000.0f)
                    );
                }
            }
        }
#endif
    }

    #if defined(__XBOX__)
        XboxLog::gXbInputMicros = XboxLog::nowMicros() - inputStart;
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Discards input events and movements.
// Should be called whenever inputs have been processed for a frame.
//------------------------------------------------------------------------------------------------------------------------------------------
void consumeEvents() noexcept {
    // Clear all events
    gKeyboardKeysJustPressed.clear();
    gKeyboardKeysJustReleased.clear();
    consumeTypedChars();

    gMouseButtonsJustPressed.clear();
    gMouseButtonsJustReleased.clear();

    gGamepadInputsJustPressed.clear();
    gGamepadInputsJustReleased.clear();
    
    gJoystickAxesJustPressed.clear();
    gJoystickAxesJustReleased.clear();
    gJoystickButtonsJustPressed.clear();
    gJoystickButtonsJustReleased.clear();
    gJoystickHatsJustPressed.clear();
    gJoystickHatsJustReleased.clear();

    // Clear all movement deltas
    static_assert(NUM_MOUSE_WHEEL_AXES == 2);
    gMouseWheelAxisMovements[0] = 0.0f;
    gMouseWheelAxisMovements[1] = 0.0f;

    consumeMouseMovements();

    // Clear other events
    gbWindowFocusJustLost = false;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Consumes all typed character events
//------------------------------------------------------------------------------------------------------------------------------------------
void consumeTypedChars() noexcept {
    gTypedChars.clear();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Consumes all current mouse movement deltas
//------------------------------------------------------------------------------------------------------------------------------------------
void consumeMouseMovements() noexcept {
    gMouseMovementX = 0;
    gMouseMovementY = 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Returns true if the user requested that the app be quit via close button
//------------------------------------------------------------------------------------------------------------------------------------------
bool isQuitRequested() noexcept {
    return gbIsQuitRequested;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Allows the ability to quit via code
//------------------------------------------------------------------------------------------------------------------------------------------
void requestQuit() noexcept {
    gbIsQuitRequested = true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Returns true if any keys or buttons are pressed.
// Note: the check does not include typed chars, those are just recorded events not input states.
//------------------------------------------------------------------------------------------------------------------------------------------
bool areAnyKeysOrButtonsPressed() noexcept {
    // Check keyboard and mouse for any button pressed
    if (!gKeyboardKeysPressed.empty())
        return true;

    if (!gMouseButtonsPressed.empty())
        return true;

    // Check game controller or generic joypad for any digital (or converted to digital) input
    if (!gGamepadInputsPressed.empty())
        return true;

    if (!gJoystickAxesPressed.empty())
        return true;

    if (!gJoystickButtonsPressed.empty())
        return true;

    if (!gJoystickHatsPressed.empty())
        return true;

    return false;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Returns the next typed character, if any.
// If there is no next typed character then the 'NUL' character is returned.
//------------------------------------------------------------------------------------------------------------------------------------------
char peekTypedChar() noexcept {
    return (gTypedChars.empty()) ? 0 : gTypedChars.front();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Pops and returns the next typed character, if any.
// If there is no next typed character then the 'NUL' character is returned.
//------------------------------------------------------------------------------------------------------------------------------------------
char popTypedChar() noexcept {
    if (gTypedChars.empty()) {
        return 0;
    } else {
        const char nextChar = gTypedChars.front();
        gTypedChars.erase(gTypedChars.begin());
        return nextChar;
    }
}

const std::vector<uint16_t>& getKeyboardKeysPressed() noexcept {
    return gKeyboardKeysPressed;
}

const std::vector<uint16_t>& getKeyboardKeysJustPressed() noexcept {
    return gKeyboardKeysJustPressed;
}

const std::vector<uint16_t>& getKeyboardKeysJustReleased() noexcept {
    return gKeyboardKeysJustReleased;
}

const std::vector<char>& getTypedChars() noexcept {
    return gTypedChars;
}

const std::vector<MouseButton>& getMouseButtonsPressed() noexcept {
    return gMouseButtonsPressed;
}

const std::vector<MouseButton>& getMouseButtonsJustPressed() noexcept {
    return gMouseButtonsJustPressed;
}

const std::vector<MouseButton>& getMouseButtonsJustReleased() noexcept {
    return gMouseButtonsJustReleased;
}

const std::vector<GamepadInput>& getGamepadInputsPressed() noexcept {
    return gGamepadInputsPressed;
}

const std::vector<GamepadInput>& getGamepadInputsJustPressed() noexcept {
    return gGamepadInputsJustPressed;
}

const std::vector<GamepadInput>& getGamepadInputsJustReleased() noexcept {
    return gGamepadInputsJustReleased;
}

const std::vector<uint32_t>& getJoystickAxesPressed() noexcept {
    return gJoystickAxesPressed;
}

const std::vector<uint32_t>& getJoystickAxesJustPressed() noexcept {
    return gJoystickAxesJustPressed;
}

const std::vector<uint32_t>& getJoystickAxesJustReleased() noexcept {
    return gJoystickAxesJustReleased;
}

const std::vector<uint32_t>& getJoystickButtonsPressed() noexcept {
    return gJoystickButtonsPressed;
}

const std::vector<uint32_t>& getJoystickButtonsJustPressed() noexcept {
    return gJoystickButtonsJustPressed;
}

const std::vector<uint32_t>& getJoystickButtonsJustReleased() noexcept {
    return gJoystickButtonsJustReleased;
}

const std::vector<JoyHat>& getJoystickHatsPressed() noexcept {
    return gJoystickHatsPressed;
}

const std::vector<JoyHat>& getJoystickHatsJustPressed() noexcept {
    return gJoystickHatsJustPressed;
}

const std::vector<JoyHat>& getJoystickHatsJustReleased() noexcept {
    return gJoystickHatsJustReleased;
}

const std::vector<JoystickAxis>& getActiveJoystickAxes() noexcept {
    return gJoystickAxes;
}

bool isKeyboardKeyPressed(const uint16_t key) noexcept {
    return vectorContainsValue(gKeyboardKeysPressed, key);
}

bool isKeyboardKeyJustPressed(const uint16_t key) noexcept {
    return vectorContainsValue(gKeyboardKeysJustPressed, key);
}

bool isKeyboardKeyReleased(const uint16_t key) noexcept {
    return (!isKeyboardKeyPressed(key));
}

bool isKeyboardKeyJustReleased(const uint16_t key) noexcept {
    return vectorContainsValue(gKeyboardKeysJustReleased, key);
}

bool isMouseButtonPressed(const MouseButton button) noexcept {
    return vectorContainsValue(gMouseButtonsPressed, button);
}

bool isMouseButtonJustPressed(const MouseButton button) noexcept {
    return vectorContainsValue(gMouseButtonsJustPressed, button);
}

bool isMouseButtonReleased(const MouseButton button) noexcept {
    return (!vectorContainsValue(gMouseButtonsPressed, button));
}

bool isMouseButtonJustReleased(const MouseButton button) noexcept {
    return vectorContainsValue(gMouseButtonsJustReleased, button);
}

bool isGamepadInputPressed(const GamepadInput input) noexcept {
    return vectorContainsValue(gGamepadInputsPressed, input);
}

bool isGamepadInputJustPressed(const GamepadInput input) noexcept {
    return vectorContainsValue(gGamepadInputsJustPressed, input);
}

bool isGamepadInputJustReleased(const GamepadInput input) noexcept {
    return vectorContainsValue(gGamepadInputsJustReleased, input);
}

bool isJoystickAxisPressed(const uint32_t axis) noexcept {
    return vectorContainsValue(gJoystickAxesPressed, axis);
}

bool isJoystickAxisJustPressed(const uint32_t axis) noexcept {
    return vectorContainsValue(gJoystickAxesJustPressed, axis);
}

bool isJoystickAxisJustReleased(const uint32_t axis) noexcept {
    return vectorContainsValue(gJoystickAxesJustReleased, axis);
}

bool isJoystickButtonPressed(const uint32_t button) noexcept {
    return vectorContainsValue(gJoystickButtonsPressed, button);
}

bool isJoystickButtonJustPressed(const uint32_t button) noexcept {
    return vectorContainsValue(gJoystickButtonsJustPressed, button);
}

bool isJoystickButtonJustReleased(const uint32_t button) noexcept {
    return vectorContainsValue(gJoystickButtonsJustReleased, button);
}

bool isJoystickHatPressed(const JoyHat hat) noexcept {
    return vectorContainsValue(gJoystickHatsPressed, hat);
}

bool isJoystickHatJustPressed(const JoyHat hat) noexcept {
    return vectorContainsValue(gJoystickHatsJustPressed, hat);
}

bool isJoystickHatJustReleased(const JoyHat hat) noexcept {
    return vectorContainsValue(gJoystickHatsJustReleased, hat);
}

float getGamepadInputValue(const GamepadInput input) noexcept {
    const uint8_t inputIdx = (uint8_t) input;
    return (inputIdx < NUM_GAMEPAD_INPUTS) ? gGamepadInputs[inputIdx] : 0.0f;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Is a second pad connected, and what does it currently read?
//
// Player two's pad is polled rather than listened to. The event path feeds one flat array of input values with no
// notion of which controller an event came from, and giving it that notion would mean touching the handling of every
// control in the game. Asking SDL for the current state of a stick or button instead is enough for a second player
// and leaves player one's path untouched.
//------------------------------------------------------------------------------------------------------------------------------------------
bool hasSecondGamepad() noexcept {
    return (gpGameController2 != nullptr);
}

static float getGamepadInputValueP2Raw(const GamepadInput input) noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Say what input devices the console actually presents.
//
// Splitscreen has been debugged by inference twice now. Whether player two's pad exists at all, and whether the pads
// come through as game controllers or as generic joysticks, decides which code path their bindings take - and those
// two paths read different state. Reporting it removes the guess.
//------------------------------------------------------------------------------------------------------------------------------------------
void logInputDevices() noexcept {
#if defined(__XBOX__)
    const int numJoysticks = SDL_NumJoysticks();

    XBOX_LOGI(
        Input,
        "input devices: %d joystick(s), p1 controller=%s p1 joystick=%s p2 controller=%s",
        numJoysticks,
        (gpGameController) ? "yes" : "no",
        (gpJoystick) ? "yes" : "no",
        (gpGameController2) ? "yes" : "no"
    );

    for (int i = 0; i < numJoysticks; ++i) {
        XBOX_LOGI(
            Input,
            "  device %d: gamecontroller=%s name='%s'",
            i,
            (SDL_IsGameController(i)) ? "yes" : "no",
            (SDL_JoystickNameForIndex(i)) ? SDL_JoystickNameForIndex(i) : "?"
        );
    }
#endif
}

float getGamepadInputValueP2(const GamepadInput input) noexcept {
    const uint8_t idx = (uint8_t) input;
    return (idx < NUM_GAMEPAD_INPUTS) ? gGamepadInputsP2[idx] : 0.0f;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Player two's digital inputs, from the sampled state.
//
// Without these, every digital control - fire, use, weapon switching, menus and pause - reads player one's pad. That
// is not merely player two being unable to act: both players then see player one's pause press, pause toggles twice in
// the same frame, and nobody can pause at all.
//------------------------------------------------------------------------------------------------------------------------------------------
bool isGamepadInputPressedP2(const GamepadInput input) noexcept {
    const uint8_t idx = (uint8_t) input;

    if (idx >= NUM_GAMEPAD_INPUTS)
        return false;

    const float threshold = Config::gAnalogToDigitalThreshold;
    const float v = gGamepadInputsP2[idx];
    return (((v >= 0.0f) ? v : -v) >= threshold);
}

bool isGamepadInputJustPressedP2(const GamepadInput input) noexcept {
    const uint8_t idx = (uint8_t) input;

    if (idx >= NUM_GAMEPAD_INPUTS)
        return false;

    const float threshold = Config::gAnalogToDigitalThreshold;
    const float now = gGamepadInputsP2[idx];
    const float was = gGamepadInputsP2Old[idx];
    const bool bNow = (((now >= 0.0f) ? now : -now) >= threshold);
    const bool bWas = (((was >= 0.0f) ? was : -was) >= threshold);
    return (bNow && (!bWas));
}

bool isGamepadInputJustReleasedP2(const GamepadInput input) noexcept {
    const uint8_t idx = (uint8_t) input;

    if (idx >= NUM_GAMEPAD_INPUTS)
        return false;

    const float threshold = Config::gAnalogToDigitalThreshold;
    const float now = gGamepadInputsP2[idx];
    const float was = gGamepadInputsP2Old[idx];
    const bool bNow = (((now >= 0.0f) ? now : -now) >= threshold);
    const bool bWas = (((was >= 0.0f) ? was : -was) >= threshold);
    return ((!bNow) && bWas);
}

static float getGamepadInputValueP2Raw(const GamepadInput input) noexcept {
    if (!gpGameController2)
        return 0.0f;

    const auto readAxis = [](SDL_GameController* const pPad, const SDL_GameControllerAxis axis) noexcept -> float {
        const int16_t raw = SDL_GameControllerGetAxis(pPad, axis);
        return (raw >= 0) ? ((float) raw / 32767.0f) : ((float) raw / 32768.0f);
    };

    const auto readButton = [](SDL_GameController* const pPad, const SDL_GameControllerButton btn) noexcept -> float {
        return (SDL_GameControllerGetButton(pPad, btn)) ? 1.0f : 0.0f;
    };

    switch (input) {
        case GamepadInput::BTN_A:               return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_A);
        case GamepadInput::BTN_B:               return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_B);
        case GamepadInput::BTN_X:               return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_X);
        case GamepadInput::BTN_Y:               return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_Y);
        case GamepadInput::BTN_BACK:            return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_BACK);
        case GamepadInput::BTN_GUIDE:           return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_GUIDE);
        case GamepadInput::BTN_START:           return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_START);
        case GamepadInput::BTN_LEFT_STICK:      return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_LEFTSTICK);
        case GamepadInput::BTN_RIGHT_STICK:     return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        case GamepadInput::BTN_LEFT_SHOULDER:   return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        case GamepadInput::BTN_RIGHT_SHOULDER:  return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        case GamepadInput::BTN_DPAD_UP:         return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_DPAD_UP);
        case GamepadInput::BTN_DPAD_DOWN:       return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        case GamepadInput::BTN_DPAD_LEFT:       return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        case GamepadInput::BTN_DPAD_RIGHT:      return readButton(gpGameController2, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        case GamepadInput::AXIS_LEFT_X:         return readAxis(gpGameController2, SDL_CONTROLLER_AXIS_LEFTX);
        case GamepadInput::AXIS_LEFT_Y:         return readAxis(gpGameController2, SDL_CONTROLLER_AXIS_LEFTY);
        case GamepadInput::AXIS_RIGHT_X:        return readAxis(gpGameController2, SDL_CONTROLLER_AXIS_RIGHTX);
        case GamepadInput::AXIS_RIGHT_Y:        return readAxis(gpGameController2, SDL_CONTROLLER_AXIS_RIGHTY);
        case GamepadInput::AXIS_TRIG_LEFT:      return readAxis(gpGameController2, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        case GamepadInput::AXIS_TRIG_RIGHT:     return readAxis(gpGameController2, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        default:                                return 0.0f;
    }
}

float getJoystickAxisValue(const uint32_t axis) noexcept {
    for (const JoystickAxis& axisAndValue : gJoystickAxes) {
        if (axisAndValue.axis == axis)
            return axisAndValue.value;
    }

    return 0.0f;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get a gamepad input which is adjusted for the given deadzone, such that the range of input values (0-1) starts just outside the deadzone.
// If the input has an opposite axis (2d axis pair), then the deadzone is treated like a circle and the following adjustment method is used:
//  https://www.gamasutra.com/blogs/JoshSutphin/20130416/190541/Doing_Thumbstick_Dead_Zones_Right.php
// If the gamepad input is not analog then no deadzone adjustments are performed.
//------------------------------------------------------------------------------------------------------------------------------------------
float getAdjustedGamepadInputValue(const GamepadInput input, const float deadZone) noexcept {
    const float rawAxis = getGamepadInputValue(input);
    const float clampedDeadZone = std::clamp(deadZone, 0.0f, 0.9999f);

    const GamepadInput oppositeInput = GamepadInputUtils::getOppositeAxis(input);
    const bool b2dAxisPair = (input != oppositeInput);

    if (b2dAxisPair) {
        // A 2d-axis pair
        const float rawAxisOpp = getGamepadInputValue(oppositeInput);
        const float axisVecLen = std::sqrt(rawAxis * rawAxis + rawAxisOpp * rawAxisOpp);

        const float axisNormalized = (axisVecLen > 0) ? rawAxis / axisVecLen : 0.0f;
        const float axisRescale = std::max((axisVecLen - clampedDeadZone) / (1.0f - clampedDeadZone), 0.0f);

        if (axisNormalized >= 0.0f) {
            return std::clamp(axisNormalized * axisRescale, 0.0f, 1.0f);
        } else {
            return std::clamp(axisNormalized * axisRescale, -1.0f, 0.0f);
        }
    }

    if (GamepadInputUtils::isAxis(input)) {
        // Simple 1d axis: just rescale based on the deadzone
        if (rawAxis >= 0) {
            return std::clamp((rawAxis - clampedDeadZone) / (1.0f - clampedDeadZone), 0.0f, 1.0f);
        } else {
            return std::clamp((rawAxis + clampedDeadZone) / (1.0f - clampedDeadZone), -1.0f, 0.0f);
        }
    }

    return rawAxis;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Same as 'getAdjustedControllerInputValue' except for a generic joystick axis.
// Because we don't know much about the axis or neighboring axes, the behavior of this might not be as good.
//------------------------------------------------------------------------------------------------------------------------------------------
float getAdjustedJoystickAxisValue(const uint32_t axis, const float deadZone) noexcept {
    const float rawAxis = getJoystickAxisValue(axis);
    const float clampedDeadZone = std::clamp(deadZone, 0.0f, 0.9999f);

    if (rawAxis >= 0) {
        return std::clamp((rawAxis - clampedDeadZone) / (1.0f - clampedDeadZone), 0.0f, 1.0f);
    } else {
        return std::clamp((rawAxis + clampedDeadZone) / (1.0f - clampedDeadZone), -1.0f, 0.0f);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the amount of mouse movement since events were last consumed (x-axis)
//------------------------------------------------------------------------------------------------------------------------------------------
float getMouseXMovement() noexcept {
    return gMouseMovementX;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the amount of mouse movement since events were last consumed (y-axis)
//------------------------------------------------------------------------------------------------------------------------------------------
float getMouseYMovement() noexcept {
    return gMouseMovementY;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the current movement amount for a mouse wheel axis
//------------------------------------------------------------------------------------------------------------------------------------------
float getMouseWheelAxisMovement(const uint8_t axis) noexcept {
    return (axis < 2) ? gMouseWheelAxisMovements[axis] : 0.0f;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Tells if the game window has just lost focus
//------------------------------------------------------------------------------------------------------------------------------------------
bool isWindowFocusJustLost() noexcept {
    return gbWindowFocusJustLost;
}

END_NAMESPACE(Input)
