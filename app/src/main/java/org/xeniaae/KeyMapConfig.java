// SPDX-License-Identifier: WTFPL

package org.xeniaae;

import android.view.KeyEvent;

public class KeyMapConfig {
    public static final int[] DEFAULT_KEYMAPPERS = new int[]{
        KeyEvent.KEYCODE_DPAD_LEFT, 
        KeyEvent.KEYCODE_DPAD_UP,
        KeyEvent.KEYCODE_DPAD_RIGHT, 
        KeyEvent.KEYCODE_DPAD_DOWN, 
        96, 97, 99, 100, 109, 108,
		102, 103, 104, 105, 0, 0,
    };

    public static final int[] KEY_NAMEIDS = new int[]{
		R.string.left,
		R.string.up,
		R.string.right,
		R.string.down,
		R.string.a,
		R.string.b,
		R.string.x,
		R.string.y,
			R.string.back,
			R.string.start,

		R.string.lshoulder,
		R.string.rshoulder,
		R.string.lthumbpress,
		R.string.rthumbpress,
		R.string.ltrigger,
		R.string.rtrigger,

        };

    public static final int[] KEY_VALUES = new int[]{
		0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,};//16,17,18,19,20,21,22,23};
}
