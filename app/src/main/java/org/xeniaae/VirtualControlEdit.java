// SPDX-License-Identifier: WTFPL
package org.xeniaae;

import android.annotation.TargetApi;
import android.app.*;
import android.os.*;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.CheckBox;
import android.widget.RadioGroup;
import android.widget.SeekBar;
import android.widget.TextView;
import android.window.OnBackInvokedDispatcher;

import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;


public class VirtualControlEdit extends Activity implements View.OnTouchListener
{
    VirtualControl vc=null;
    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        // TODO: Implement this method
        super.onCreate(savedInstanceState);
        Utils.enable_fullscreen(getWindow());

        setContentView(vc=VirtualControl.Edit( this));
        vc.setOnTouchListener(this);

        if(Build.VERSION.SDK_INT>=33){
            reg_onBackPressed();
        }
    }

    @TargetApi(33)
    void reg_onBackPressed(){
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                ()->{
                    create_option_menu();
                }
        );
    }

    void create_option_menu(){
        final Dialog d=new AlertDialog.Builder(this, androidx.appcompat.R.style.Theme_AppCompat_Light_Dialog_Alert)
                .setView(R.layout.pad_edit_menu)
                .create();

        d.show();

        ((CheckBox)d.findViewById(R.id.virtual_pad_disable)).setEnabled(false);
        /*((CheckBox)d.findViewById(R.id.virtual_pad_disable)).setChecked(!iv.get_all_input_enabled());
        ((CheckBox)d.findViewById(R.id.virtual_pad_disable)).setOnCheckedChangeListener((buttonView, isChecked)->{
                    boolean enabled=!isChecked;
                    iv.set_all_input_enabled(enabled);
                }
        );*/

        d.findViewById(R.id.virtual_pad_reset).setOnClickListener(v->{
            vc.load_config(vc.default_config());
            vc.invalidate();
            d.dismiss();
        });

        d.findViewById(R.id.virtual_pad_save_quit).setOnClickListener(v->{
            d.dismiss();
            vc.save_config(Application.get_virtual_control_config_file());
            finish();
        });

        final String scale_text=getString(R.string.scale_rate)+": ";
        double joystick_scale=vc.find_component("joystick_l").get_scale();
        double dpad_scale=vc.find_component("dpad").get_scale();
        double abxy_scale=vc.find_component("a").get_scale();
        double sb_scale=vc.find_component("start").get_scale();
        double lr_scale=vc.find_component("shoulder_l").get_scale();

        ((TextView)d.findViewById(R.id.virtual_pad_joystick_scale_hint)).setText(scale_text+joystick_scale);
        ((TextView)d.findViewById(R.id.virtual_pad_dpad_scale_hint)).setText(scale_text+dpad_scale);
        ((TextView)d.findViewById(R.id.virtual_pad_button_scale_hint)).setText(scale_text+abxy_scale);
        ((TextView)d.findViewById(R.id.virtual_pad_ss_scale_hint)).setText(scale_text+sb_scale);
        ((TextView)d.findViewById(R.id.virtual_pad_lr_scale_hint)).setText(scale_text+lr_scale);

        ((SeekBar)d.findViewById(R.id.virtual_pad_joystick_scale)).setProgress((int)(joystick_scale*100));
        ((SeekBar)d.findViewById(R.id.virtual_pad_joystick_scale)).setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            double scale;
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                scale=progress/100.f;
                ((TextView)d.findViewById(R.id.virtual_pad_joystick_scale_hint)).setText(scale_text+scale);
            }
            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                vc.find_component("joystick_l").set_scale( scale);
                vc.find_component("joystick_r").set_scale( scale);
                vc.invalidate();
            }
        });

        ((SeekBar)d.findViewById(R.id.virtual_pad_dpad_scale)).setProgress((int)(dpad_scale*100));
        ((SeekBar)d.findViewById(R.id.virtual_pad_dpad_scale)).setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            double scale;
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                scale=progress/100.f;
                ((TextView)d.findViewById(R.id.virtual_pad_dpad_scale_hint)).setText(scale_text+scale);
            }
            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                vc.find_component("dpad").set_scale( scale);
                vc.invalidate();
            }
        });

        ((SeekBar)d.findViewById(R.id.virtual_pad_button_scale)).setProgress((int)(abxy_scale*100));
        ((SeekBar)d.findViewById(R.id.virtual_pad_button_scale)).setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            double scale;
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                scale=progress/100.f;
                ((TextView)d.findViewById(R.id.virtual_pad_button_scale_hint)).setText(scale_text+scale);
            }
            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                vc.find_component("a").set_scale( scale);
                vc.find_component("b").set_scale( scale);
                vc.find_component("x").set_scale( scale);
                vc.find_component("y").set_scale( scale);
                vc.invalidate();
            }
        });

        ((SeekBar)d.findViewById(R.id.virtual_pad_ss_scale)).setProgress((int)(sb_scale*100));
        ((SeekBar)d.findViewById(R.id.virtual_pad_ss_scale)).setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            double scale;
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                scale=progress/100.f;
                ((TextView)d.findViewById(R.id.virtual_pad_ss_scale_hint)).setText(scale_text+scale);
            }
            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                vc.find_component("start").set_scale( scale);
                vc.find_component("back").set_scale( scale);
                vc.invalidate();
            }
        });

        ((SeekBar)d.findViewById(R.id.virtual_pad_lr_scale)).setProgress((int)(lr_scale*100));
        ((SeekBar)d.findViewById(R.id.virtual_pad_lr_scale)).setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            double scale;
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                scale=progress/100.f;
                ((TextView)d.findViewById(R.id.virtual_pad_lr_scale_hint)).setText(scale_text+scale);
            }
            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                vc.find_component("shoulder_l").set_scale( scale);
                vc.find_component("shoulder_r").set_scale( scale);
                vc.find_component("thumb_press_l").set_scale( scale);
                vc.find_component("thumb_press_r").set_scale( scale);
                vc.find_component("trigger_l").set_scale( scale);
                vc.find_component("trigger_r").set_scale( scale);
                vc.invalidate();
            }
        });
    }

    @Override
    public void onBackPressed()
    {
        create_option_menu();
    }

    VirtualControl.Component touch_component;
    @Override
    public boolean onTouch(View v, MotionEvent event) {
        switch (event.getAction()) {
            case MotionEvent.ACTION_DOWN:
                touch_component=vc.find_component_by_touch_point( (int)event.getX(), (int)event.getY());
                break;
                case MotionEvent.ACTION_MOVE:
                if(touch_component!=null){
                    vc.set_component_position(touch_component, (int)event.getX(), (int)event.getY());
                    vc.invalidate();
                }
                break;
                case MotionEvent.ACTION_UP:
                if(touch_component!=null){
                    vc.set_component_position(touch_component, (int)event.getX(), (int)event.getY());
                    vc.invalidate();
                }
                touch_component=null;
                break;
        }
        return true;
    }
}
