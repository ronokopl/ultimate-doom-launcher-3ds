// Gyro + touch aiming (Subsystems sheet: aim; settings: Menu sheet, 3DS Options).
// Both feed the engine like a mouse (G_AddViewAngle / G_AddViewPitch, 65536 units per turn), with fractional
// remainders kept so slow movements are not lost.
//   gyro_mode: 0 off, 1 hold button, 2 toggle button, 3 always on (holding the button pauses it: ratchet)
//   the button: gyro_button = a 3DS button read straight from the hardware (bd3ds_gyro_buttons), so it can
//   share a button that is also bound to something else (e.g. aim down sights); the +gyro command works too
//   axes: gyro_axis_lr / gyro_axis_ud choose which motion drives left/right and up/down - yaw = turning the
//   console, pitch = tilting it forward/back, roll = tilting it sideways (physical names; libctru's angularRate
//   field names differ) - each direction has its own invert
//   drift: gyro_autocalib learns each sensor's resting offset while the readings are steady (see LearnBias)
//   touch: the bottom screen's aim areas (BottomScreen sheet, kind aim) drag the view like a laptop touchpad
#include <3ds.h>
#include <math.h>

#include "doomtype.h"
#include "doomdef.h"
#include "templates.h"
#include "doomstat.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "g_game.h"
#include "menu/menu.h"
#include "c_console.h"
#include "i_3ds.h"
#include "bd3ds_sheets.h"

CVAR (Int,   gyro_mode,         BD3DS_DEFAULT_GYRO_MODE,     CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR (Int,   gyro_button,       BD3DS_DEFAULT_GYRO_BUTTON,     CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// index into bd3ds_gyro_buttons (0 = none)
CVAR (Int,   gyro_axis_lr,      BD3DS_DEFAULT_GYRO_AXIS_LR,    CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// left/right from 0 yaw, 1 roll, 2 pitch
CVAR (Int,   gyro_axis_ud,      BD3DS_DEFAULT_GYRO_AXIS_UD,    CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// up/down from 0 pitch, 1 roll, 2 yaw, 3 off
CVAR (Bool,  gyro_invert_lr,    BD3DS_DEFAULT_GYRO_INVERT_LR,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR (Bool,  gyro_invert_ud,    BD3DS_DEFAULT_GYRO_INVERT_UD,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR (Bool,  gyro_autocalib,    BD3DS_DEFAULT_GYRO_AUTOCALIB,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// drift correction
CVAR (Float, gyro_sens_x,       BD3DS_DEFAULT_GYRO_SENS_X,   CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// view degrees per degree of rotation
CVAR (Float, gyro_sens_y,       BD3DS_DEFAULT_GYRO_SENS_Y,   CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR (Float, gyro_deadzone,     1.0f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// deg/s ignored after the drift correction
CVAR (Bool,  touch_aim,      BD3DS_DEFAULT_TOUCH_AIM, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// status screen only; other modes always aim
CVAR (Float, touch_sens_x,   BD3DS_DEFAULT_TOUCH_SENS_X,   CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// 1.0 = 0.5 view degrees per pixel
CVAR (Float, touch_sens_y,   BD3DS_DEFAULT_TOUCH_SENS_Y,   CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR (Bool,  touch_invert_y, BD3DS_DEFAULT_TOUCH_INVERT_Y, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static bool GyroButton, GyroToggled, GyroOn;	// GyroButton = +gyro held
static bool PadWasHeld;							// gyro_button state last poll
static float GyroCoeff = 14.375f;	// raw units per deg/s (HID reports the real value)
static u64 LastTick;
static float PendingYaw, PendingPitch;	// view degrees not yet sent
static float Bias[3], Smooth[3];	// per sensor (angularRate x, y, z): learned resting offset, ~0.1 s average (deg/s)
static float SteadyTime, CalibTime;

// "+gyro" / "-gyro": a key bound to a +command runs the -command on release (C_DoKey)
static void GyroPress(FCommandLine &, APlayerPawn *, int)
{
	GyroButton = true;
	if (gyro_mode == 2)
		GyroToggled = !GyroToggled;
}

static void GyroRelease(FCommandLine &, APlayerPawn *, int)
{
	GyroButton = false;
}

static FConsoleCommand Cmd_PlusGyro("+gyro", GyroPress);
static FConsoleCommand Cmd_MinusGyro("-gyro", GyroRelease);

static bool AimAllowed()
{
	return gamestate == GS_LEVEL && menuactive == MENU_Off && ConsoleState == c_up && !paused;
}

// Adds view rotation in degrees (positive yaw = turn right, positive pitch = look down).
void I3DS_AimAdd(float yawdeg, float pitchdeg)
{
	if (!AimAllowed())
	{
		PendingYaw = PendingPitch = 0;
		return;
	}
	PendingYaw += yawdeg * (65536.f / 360.f);
	PendingPitch += pitchdeg * (65536.f / 360.f);
	int yaw = (int)PendingYaw, pitch = (int)PendingPitch;
	PendingYaw -= yaw;
	PendingPitch -= pitch;
	if (yaw)
		G_AddViewAngle(yaw);
	if (pitch)
		G_AddViewPitch(-pitch);
}

// Touch drag in bottom-screen pixels.
void I3DS_AimTouch(int dx, int dy)
{
	I3DS_AimAdd(dx * 0.5f * touch_sens_x, dy * 0.5f * touch_sens_y * (touch_invert_y ? -1.f : 1.f));
}

bool I3DS_TouchAimEnabled()
{
	return touch_aim;
}

static void SetGyroHardware(bool on)
{
	if (on == GyroOn)
		return;
	if (on)
	{
		HIDUSER_EnableGyroscope();
		SteadyTime = 0;
		for (int i = 0; i < 3; ++i)
			Smooth[i] = Bias[i];	// the offset learned so far stays (it changes slowly with temperature)
		float c = 0;
		if (R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&c)) && c > 0.f)
			GyroCoeff = c;
	}
	else
		HIDUSER_DisableGyroscope();
	GyroOn = on;
}

void I3DS_AimSuspend()
{
	SetGyroHardware(false);	// shared with the system: not left running while the HOME Menu takes over
}

// Drift correction: a gyro reads a small rate (a few deg/s) while it is held still, which turned the view
// slowly (the aim crept downwards). Whenever the readings are steady - little jitter around their ~0.1 s average,
// which stays near the current offset estimate - for half a second, the estimate moves towards that average:
// fast for the first seconds, then slowly, and slowest while the gyro is aiming, so deliberate slow aiming is
// hardly absorbed. The deadzone applies after the correction.
static void LearnBias(const float rate[3], float dt, bool aiming)
{
	const float window = CalibTime < 2.f ? 10.f : 5.f;	// deg/s an offset may be away from the estimate
	bool steady = true;
	for (int i = 0; i < 3; ++i)
	{
		Smooth[i] += (rate[i] - Smooth[i]) * MIN(1.f, dt / 0.1f);
		if (fabsf(rate[i] - Smooth[i]) > 2.f || fabsf(Smooth[i] - Bias[i]) > window)
			steady = false;
	}
	SteadyTime = steady ? SteadyTime + dt : 0.f;
	if (SteadyTime < 0.5f)
		return;
	float tau = CalibTime < 2.f ? 0.5f : aiming ? 8.f : 2.f;	// seconds to close ~63% of the gap
	for (int i = 0; i < 3; ++i)
		Bias[i] += (Smooth[i] - Bias[i]) * MIN(1.f, dt / tau);
	CalibTime += dt;
}

// Called every input poll (after hidScanInput).
void I3DS_AimUpdate()
{
	u64 now = svcGetSystemTick();
	float dt = LastTick ? (now - LastTick) / (float)SYSCLOCK_ARM11 : 0.f;
	LastTick = now;
	SetGyroHardware(gyro_mode != 0);
	int b = gyro_button;
	u32 mask = b > 0 && b < BD3DS_GYRO_BUTTON_COUNT ? bd3ds_gyro_buttons[b] : 0;
	bool pad = mask != 0 && (hidKeysHeld() & mask) != 0;
	if (pad && !PadWasHeld && gyro_mode == 2 && AimAllowed())
		GyroToggled = !GyroToggled;
	PadWasHeld = pad;
	if (!GyroOn || dt <= 0.f || dt > 0.25f)
		return;
	bool held = GyroButton || pad;
	bool active = gyro_mode == 1 ? held : gyro_mode == 2 ? GyroToggled : gyro_mode == 3 ? !held : false;
	angularRate r;
	hidGyroRead(&r);
	float rate[3] = { r.x / GyroCoeff, r.y / GyroCoeff, r.z / GyroCoeff };	// deg/s per sensor
	if (gyro_autocalib)
		LearnBias(rate, dt, active);
	if (!active)
		return;
	for (int i = 0; i < 3; ++i)
	{
		float v = rate[i] - (gyro_autocalib ? Bias[i] : 0.f);
		rate[i] = fabsf(v) < gyro_deadzone ? 0.f : v;
	}
	// physical motions with the console held normally, signed as view rates (yaw: + turns right, pitch: + looks
	// down): turning = sensor y, tilting forward/back = sensor x, tilting sideways = sensor z (libctru calls
	// these pitch, roll and yaw). Yaw + pitch reproduce the setup tested on hardware.
	const float motion[3] = { -rate[1], -rate[2], rate[0] };	// yaw, roll, pitch
	static const int lrmap[3] = { 0, 1, 2 }, udmap[4] = { 2, 1, 0, -1 };	// option index -> motion[] index
	int h = gyro_axis_lr, v = gyro_axis_ud;
	float hrate = h >= 0 && h < 3 ? motion[lrmap[h]] : 0.f;
	float vrate = v >= 0 && v < 3 ? motion[udmap[v]] : 0.f;
	I3DS_AimAdd((gyro_invert_lr ? -hrate : hrate) * dt * gyro_sens_x, (gyro_invert_ud ? -vrate : vrate) * dt * gyro_sens_y);
}

void I3DS_AimShutdown()
{
	SetGyroHardware(false);
}
