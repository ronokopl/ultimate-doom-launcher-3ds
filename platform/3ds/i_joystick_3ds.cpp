// 3DS analog sticks as one ZDoom joystick (replaces posix/sdl/i_joystick.cpp). Subsystems sheet: joystick.
// Axes follow the Controls sheet rows CIRCLEPAD (move/strafe) and CSTICK (aim); all remappable in the menu.
#include <3ds.h>

#include "doomdef.h"
#include "templates.h"
#include "m_joy.h"

#define MIN_DEADZONE 0.000001f
#define PAD_DEADZONE 0.12f		// circle pads jitter around the center

class C3DSPad : public IJoystickConfig
{
public:
	C3DSPad() : Multiplier(1.0f)
	{
		SetDefaultConfig();
	}
	~C3DSPad()
	{
		M_SaveJoystickConfig(this);
	}

	FString GetName() { return "3DS Circle Pad + C-Stick"; }
	float GetSensitivity() { return Multiplier; }
	void SetSensitivity(float scale) { Multiplier = scale; }

	int GetNumAxes() { return NUM_AXES; }
	float GetAxisDeadZone(int axis) { return Axes[axis].DeadZone; }
	EJoyAxis GetAxisMap(int axis) { return Axes[axis].GameAxis; }
	const char *GetAxisName(int axis) { return Names[axis]; }
	float GetAxisScale(int axis) { return Axes[axis].Multiplier; }

	void SetAxisDeadZone(int axis, float zone) { Axes[axis].DeadZone = clamp(zone, MIN_DEADZONE, 1.f); }
	void SetAxisMap(int axis, EJoyAxis gameaxis) { Axes[axis].GameAxis = gameaxis; }
	void SetAxisScale(int axis, float scale) { Axes[axis].Multiplier = scale; }

	bool IsSensitivityDefault() { return Multiplier == 1.0f; }
	bool IsAxisDeadZoneDefault(int axis) { return Axes[axis].DeadZone == PAD_DEADZONE; }
	bool IsAxisMapDefault(int axis) { return Axes[axis].GameAxis == DefaultAxes[axis]; }
	bool IsAxisScaleDefault(int axis) { return Axes[axis].Multiplier == 1.0f; }

	void SetDefaultConfig()
	{
		for (int i = 0; i < NUM_AXES; i++)
		{
			Axes[i].DeadZone = PAD_DEADZONE;
			Axes[i].Multiplier = 1.0f;
			Axes[i].Value = 0.0;
			Axes[i].ButtonValue = 0;
			Axes[i].GameAxis = DefaultAxes[i];
		}
	}
	FString GetIdentifier() { return "JS:3DS"; }

	void AddAxes(float axes[NUM_JOYAXIS])
	{
		for (int i = 0; i < NUM_AXES; ++i)
		{
			if (Axes[i].GameAxis != JOYAXIS_None)
				axes[Axes[i].GameAxis] -= float(Axes[i].Value * Multiplier * Axes[i].Multiplier);
		}
	}

	void ProcessInput()
	{
		circlePosition cp, cs;
		hidCircleRead(&cp);
		hidCstickRead(&cs);
		// SDL convention: +x = right, +y = down.
		double raw[NUM_AXES] = { cp.dx / 156.0, -cp.dy / 156.0, cs.dx / 146.0, -cs.dy / 146.0 };
		for (int i = 0; i < NUM_AXES; ++i)
		{
			BYTE buttonstate = 0;
			Axes[i].Value = Joy_RemoveDeadZone(clamp(raw[i], -1.0, 1.0), Axes[i].DeadZone, &buttonstate);
			if (i >= 2)
			{
				Joy_GenerateButtonEvents(Axes[i].ButtonValue, buttonstate, 2, KEY_JOYAXIS1PLUS + i*2);
				Axes[i].ButtonValue = buttonstate;
			}
		}
		// menu directions: Joy_XYAxesToButtons wants (vertical, horizontal) - axis-1 buttons are up/down in the
		// menu (as the DirectInput path passes them); x first swapped up/down with left/right on the circle pad
		BYTE xy = Joy_XYAxesToButtons(Axes[1].Value, Axes[0].Value);
		Joy_GenerateButtonEvents(Axes[0].ButtonValue, xy, 4, KEY_JOYAXIS1PLUS);
		Axes[0].ButtonValue = xy;
	}

private:
	enum { NUM_AXES = 4 };
	struct AxisInfo
	{
		float DeadZone;
		float Multiplier;
		EJoyAxis GameAxis;
		double Value;
		BYTE ButtonValue;
	};
	static const EJoyAxis DefaultAxes[NUM_AXES];
	static const char *const Names[NUM_AXES];
	float Multiplier;
	AxisInfo Axes[NUM_AXES];
};
const EJoyAxis C3DSPad::DefaultAxes[NUM_AXES] = { JOYAXIS_Side, JOYAXIS_Forward, JOYAXIS_Yaw, JOYAXIS_Pitch };
const char *const C3DSPad::Names[NUM_AXES] = { "Circle Pad X", "Circle Pad Y", "C-Stick X", "C-Stick Y" };

static C3DSPad *Pad;

void I_StartupJoysticks()
{
	if (Pad == NULL)
		Pad = new C3DSPad;
}

void I_ShutdownJoysticks()
{
	if (Pad != NULL)
	{
		delete Pad;
		Pad = NULL;
	}
}

void I_GetJoysticks(TArray<IJoystickConfig *> &sticks)
{
	sticks.Clear();
	if (Pad != NULL)
	{
		M_LoadJoystickConfig(Pad);
		sticks.Push(Pad);
	}
}

void I_GetAxes(float axes[NUM_JOYAXIS])
{
	for (int i = 0; i < NUM_JOYAXIS; ++i)
		axes[i] = 0;
	if (use_joystick && Pad != NULL)
		Pad->AddAxes(axes);
}

void I_ProcessJoysticks()
{
	if (use_joystick && Pad != NULL)
		Pad->ProcessInput();
}

IJoystickConfig *I_UpdateDeviceList()
{
	return NULL;
}
