package org.xeniaae;

import android.content.Context;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;
import android.view.InputDevice;
import android.view.KeyEvent;

import java.util.ArrayList;
import java.util.List;

/**
 * Detects connected gamepads and maps them without the user touching Settings.
 *
 * <p>Why this exists: the key map is a flat list of Android keycodes stored in
 * shared preferences, and until now it only ever held one hard-coded default
 * set. That default assumes an Xbox-style pad. Anything that reports different
 * keycodes — most obviously Nintendo-layout pads, where the physical A/B and
 * X/Y positions are swapped relative to their keycodes — came up wrong, and the
 * only fix was to re-bind all sixteen buttons by hand.
 *
 * <p>The mapping we write is always expressed as "which Android keycode drives
 * which guest button", which is exactly what {@code EmulatorActivity} reads
 * back, so an auto-mapped pad and a hand-mapped one are indistinguishable
 * afterwards. That also means the user can still override any button later.
 */
public final class ControllerAutoMap {

    /** Remembers which devices we have already mapped, so we only do it once. */
    private static final String PREF_MAPPED_DEVICES = "automap_mapped_devices";

    private ControllerAutoMap() {}

    /** A connected pad plus the profile we matched it to. */
    public static final class Pad {
        public final int deviceId;
        public final String name;
        public final String descriptor;
        public final String profile;

        Pad(int deviceId, String name, String descriptor, String profile) {
            this.deviceId = deviceId;
            this.name = name;
            this.descriptor = descriptor;
            this.profile = profile;
        }
    }

    // ------------------------------------------------------------- detection

    /** True for real gamepads/joysticks, excluding the touchscreen and keyboards. */
    private static boolean isGamepad(InputDevice dev) {
        if (dev == null || dev.isVirtual()) {
            return false;
        }
        int sources = dev.getSources();
        boolean gamepad = (sources & InputDevice.SOURCE_GAMEPAD)
                == InputDevice.SOURCE_GAMEPAD;
        boolean joystick = (sources & InputDevice.SOURCE_JOYSTICK)
                == InputDevice.SOURCE_JOYSTICK;
        return gamepad || joystick;
    }

    /** Every gamepad currently attached. */
    public static List<Pad> connectedPads() {
        List<Pad> pads = new ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice dev = InputDevice.getDevice(id);
            if (!isGamepad(dev)) {
                continue;
            }
            pads.add(new Pad(id, dev.getName(), dev.getDescriptor(), profileFor(dev)));
        }
        return pads;
    }

    /**
     * Picks a layout profile for a pad.
     *
     * <p>Vendor IDs are checked before names because names are wildly
     * inconsistent between OEM firmware revisions, whereas the USB/BT vendor ID
     * is stable. The name check is only a fallback for pads that report a
     * generic vendor.
     */
    private static String profileFor(InputDevice dev) {
        int vendor = dev.getVendorId();
        switch (vendor) {
            case 0x057E:  // Nintendo
                return "nintendo";
            case 0x054C:  // Sony
                return "playstation";
            case 0x045E:  // Microsoft
                return "xbox";
            default:
                break;
        }
        String n = dev.getName() == null ? "" : dev.getName().toLowerCase();
        if (n.contains("nintendo") || n.contains("switch") || n.contains("joy-con")) {
            return "nintendo";
        }
        if (n.contains("dualsense") || n.contains("dualshock") || n.contains("playstation")
                || n.contains("wireless controller")) {
            return "playstation";
        }
        if (n.contains("xbox") || n.contains("xinput")) {
            return "xbox";
        }
        return "generic";
    }

    // --------------------------------------------------------------- mapping

    /**
     * Keycodes for a profile, in {@link KeyMapConfig#KEY_NAMEIDS} order.
     *
     * <p>Only Nintendo-layout pads actually need to differ: Android reports the
     * button that is physically where Xbox's B sits as {@code BUTTON_A}, so
     * using the stock table makes every menu confirm/cancel feel inverted.
     * Swapping A/B and X/Y restores the physical positions the guest expects.
     */
    private static int[] mappingFor(String profile) {
        int[] m = KeyMapConfig.DEFAULT_KEYMAPPERS.clone();
        if ("nintendo".equals(profile)) {
            // indices 4..7 are A, B, X, Y
            m[4] = KeyEvent.KEYCODE_BUTTON_B;
            m[5] = KeyEvent.KEYCODE_BUTTON_A;
            m[6] = KeyEvent.KEYCODE_BUTTON_Y;
            m[7] = KeyEvent.KEYCODE_BUTTON_X;
        }
        return m;
    }

    /**
     * Applies a pad's profile to the stored key map.
     *
     * <p>Buttons the device does not physically report are left at their
     * existing binding rather than being written blind: a pad without L3/R3
     * should not silently claim those keycodes and shadow another input.
     */
    private static void applyMapping(Context ctx, InputDevice dev, String profile) {
        int[] mapping = mappingFor(profile);
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(ctx);
        SharedPreferences.Editor ed = prefs.edit();

        boolean[] present = dev.hasKeys(mapping);
        for (int i = 0; i < KeyMapConfig.KEY_NAMEIDS.length && i < mapping.length; i++) {
            if (mapping[i] == 0) {
                continue;
            }
            // Some inputs do not arrive as KEYS at all and hasKeys() reports
            // false for them, but they still need a binding:
            //   * D-pad     - usually the HAT_X/HAT_Y axes.
            //   * triggers  - usually the LTRIGGER/RTRIGGER (or BRAKE/GAS)
            //                 axes; the keycode is only a fallback for pads
            //                 that also send BUTTON_L2/R2.
            // Skipping those left the triggers unmapped, which is exactly the
            // hole this class was added to close.
            boolean alwaysMap = i < 4 || i >= 14;
            if (!alwaysMap && i < present.length && !present[i]) {
                continue;
            }
            ed.putInt(Integer.toString(KeyMapConfig.KEY_NAMEIDS[i]), mapping[i]);
        }
        ed.apply();
    }

    /**
     * Maps every connected pad that has not been mapped before.
     *
     * @return the pad that was newly mapped, or null if there was nothing to do.
     */
    public static Pad autoMapNewDevices(Context ctx) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(ctx);
        String seen = prefs.getString(PREF_MAPPED_DEVICES, "");
        Pad mapped = null;

        for (Pad pad : connectedPads()) {
            // Descriptor is stable across reconnects and reboots; device id is not.
            String token = pad.descriptor;
            if (token == null || token.isEmpty() || seen.contains(token)) {
                continue;
            }
            InputDevice dev = InputDevice.getDevice(pad.deviceId);
            if (dev == null) {
                continue;
            }
            applyMapping(ctx, dev, pad.profile);
            seen = seen.isEmpty() ? token : seen + "|" + token;
            mapped = pad;
        }
        if (mapped != null) {
            prefs.edit().putString(PREF_MAPPED_DEVICES, seen).apply();
        }
        return mapped;
    }

    /**
     * One-time repair of key maps saved before the trigger/thumb fix.
     *
     * <p>The old defaults bound the thumbstick clicks to 104/105 - which are
     * actually BUTTON_L2/BUTTON_R2, the triggers - and left the triggers
     * themselves unmapped at 0. Anyone who ran an earlier build has those values
     * persisted, so shipping corrected defaults alone would only help fresh
     * installs. Worse, a user who then bound the triggers by hand to 104/105
     * ended up with two guest buttons on one keycode, where the runtime map
     * silently keeps just one.
     *
     * <p>Only the four affected entries are touched, and only when they still
     * hold the broken values, so a deliberate custom binding is left alone.
     */
    public static void migrateLegacyKeyMap(Context ctx) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(ctx);
        final int L_THUMB = 12, R_THUMB = 13, L_TRIG = 14, R_TRIG = 15;
        String kLThumb = Integer.toString(KeyMapConfig.KEY_NAMEIDS[L_THUMB]);
        String kRThumb = Integer.toString(KeyMapConfig.KEY_NAMEIDS[R_THUMB]);
        String kLTrig = Integer.toString(KeyMapConfig.KEY_NAMEIDS[L_TRIG]);
        String kRTrig = Integer.toString(KeyMapConfig.KEY_NAMEIDS[R_TRIG]);

        int lThumb = prefs.getInt(kLThumb, KeyEvent.KEYCODE_BUTTON_THUMBL);
        int rThumb = prefs.getInt(kRThumb, KeyEvent.KEYCODE_BUTTON_THUMBR);
        int lTrig = prefs.getInt(kLTrig, KeyEvent.KEYCODE_BUTTON_L2);
        int rTrig = prefs.getInt(kRTrig, KeyEvent.KEYCODE_BUTTON_R2);

        boolean thumbsWrong = lThumb == KeyEvent.KEYCODE_BUTTON_L2
                || rThumb == KeyEvent.KEYCODE_BUTTON_R2;
        boolean triggersUnset = lTrig == 0 || rTrig == 0;
        boolean collision = lTrig == lThumb || rTrig == rThumb;
        if (!thumbsWrong && !triggersUnset && !collision) {
            return;
        }
        prefs.edit()
                .putInt(kLThumb, KeyEvent.KEYCODE_BUTTON_THUMBL)
                .putInt(kRThumb, KeyEvent.KEYCODE_BUTTON_THUMBR)
                .putInt(kLTrig, KeyEvent.KEYCODE_BUTTON_L2)
                .putInt(kRTrig, KeyEvent.KEYCODE_BUTTON_R2)
                .apply();
    }

    /** Forgets all remembered pads so the next connect re-maps from scratch. */
    public static void forgetAll(Context ctx) {
        PreferenceManager.getDefaultSharedPreferences(ctx)
                .edit().remove(PREF_MAPPED_DEVICES).apply();
    }

    /** Human-readable profile name for the Settings list. */
    public static String profileLabel(String profile) {
        switch (profile) {
            case "xbox":
                return "Xbox layout";
            case "playstation":
                return "PlayStation layout";
            case "nintendo":
                return "Nintendo layout (A/B, X/Y swapped)";
            default:
                return "Standard layout";
        }
    }
}
