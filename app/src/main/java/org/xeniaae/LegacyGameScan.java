package org.xeniaae;

import android.content.ContentUris;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.MediaStore;

import androidx.documentfile.provider.DocumentFile;

import java.util.ArrayList;

/**
 * Reproduces the auto-discovery behavior of the old file-list UI (before the
 * explicit "Add Games" library was introduced): scan a configured folder, or
 * fall back to Downloads for *.iso. Used once by MainActivity to migrate
 * existing users' already-detected games into the new persisted library.
 */
class LegacyGameScan {

    private static boolean is_god_game(String file_name){
        return file_name.indexOf('.')==-1;
    }

    private static boolean is_iso_file(String file_name){
        return file_name.endsWith(".iso");
    }

    private static boolean is_zar_file(String file_name){
        return file_name.endsWith(".zar");
    }

    private static DocumentFile get_default_xex_file(DocumentFile dir){
        DocumentFile[] files=dir.listFiles();
        if(files == null) return null;
        if(files.length == 0) return null;
        for(DocumentFile file:files){
            if(!file.isFile()) continue;
            if(file.getName().toLowerCase().equals("default.xex")) return file;
        }
        return null;
    }

    static ArrayList<Emulator.GameInfo> scan_downloads_for_isos(Context context) {
        ArrayList<Emulator.GameInfo> metas = new ArrayList<>();
        if (Build.VERSION.SDK_INT < 29) return metas;
        Uri collection = MediaStore.Downloads.getContentUri(MediaStore.VOLUME_EXTERNAL);
        String[] projection = {
            MediaStore.Downloads._ID,
            MediaStore.Downloads.DISPLAY_NAME
        };
        String selection = MediaStore.Downloads.DISPLAY_NAME + " LIKE ?";
        String[] args = {"%.iso"};
        try (Cursor cursor = context.getContentResolver().query(
                collection, projection, selection, args, null)) {
            if (cursor == null) return metas;
            int idCol = cursor.getColumnIndexOrThrow(MediaStore.Downloads._ID);
            int nameCol = cursor.getColumnIndexOrThrow(MediaStore.Downloads.DISPLAY_NAME);
            while (cursor.moveToNext()) {
                long id = cursor.getLong(idCol);
                String name = cursor.getString(nameCol);
                Uri uri = ContentUris.withAppendedId(collection, id);
                Emulator.GameInfo meta = new Emulator.GameInfo();
                meta.name = name.endsWith(".iso")
                    ? name.substring(0, name.length() - 4) : name;
                meta.uri = uri.toString();
                metas.add(meta);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        return metas;
    }

    /** {@code gameDirUri} may be null, in which case Downloads is scanned for *.iso. */
    static ArrayList<Emulator.GameInfo> scan(Context context, Uri gameDirUri){
        ArrayList<Emulator.GameInfo> metas=new ArrayList<Emulator.GameInfo>();
        if(gameDirUri==null)
            return scan_downloads_for_isos(context);
        DocumentFile iso_dir=DocumentFile.fromTreeUri(context, gameDirUri);
        if(iso_dir==null||!iso_dir.exists())
            return metas;
        DocumentFile[] files=iso_dir.listFiles();
        for(DocumentFile file:files){
            if(file.isDirectory()){
                DocumentFile default_xex_file=get_default_xex_file(file);
                if(default_xex_file==null) continue;
                Emulator.GameInfo meta=new Emulator.GameInfo();
                meta.uri=default_xex_file.getUri().toString();
                meta.name=file.getName();
                metas.add(meta);
            }
            else{
                if(is_iso_file(file.getName())){
                    Emulator.GameInfo meta=new Emulator.GameInfo();
                    meta.name=file.getName().substring(0,file.getName().length()-4);
                    meta.uri=file.getUri().toString();
                    metas.add(meta);
                }
                if(is_zar_file(file.getName())){
                    Emulator.GameInfo meta=new Emulator.GameInfo();
                    meta.name=file.getName().substring(0,file.getName().length()-4);
                    meta.uri=file.getUri().toString();
                    metas.add(meta);
                }
                else if(is_god_game(file.getName())){
                    Emulator.GameInfo meta=Emulator.get.meta_info_from_god_game(context,file.getUri().toString());
                    if(meta!=null)
                    metas.add(meta);
                }
            }
        }
        return metas;
    }
}
