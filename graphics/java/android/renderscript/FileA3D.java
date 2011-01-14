/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.renderscript;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;

import android.content.res.AssetManager;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.Log;
import android.util.TypedValue;

/**
 * FileA3D allows users to load renderscript objects from files
 * or resources stored on disk. It could be used to load items
 * such as 3d geometry data converted a renderscript format from
 * content creation tools. Currently only meshes are supported
 * in FileA3D.
 *
 * When successfully loaded, FileA3D will contain a list of
 * index entries for all the objects stored inside it.
 *
 **/
public class FileA3D extends BaseObj {

    /**
    * Specifies what renderscript object type is contained within
    * the FileA3D IndexEntry
    **/
    public enum EntryType {

        UNKNOWN (0),
        MESH (1);

        int mID;
        EntryType(int id) {
            mID = id;
        }

        static EntryType toEntryType(int intID) {
            return EntryType.values()[intID];
        }
    }

    /**
    * IndexEntry contains information about one of the renderscript
    * objects inside the file's index. It could be used to query the
    * object's type and name and load the object itself if
    * necessary.
    */
    public static class IndexEntry {
        RenderScript mRS;
        int mIndex;
        int mID;
        String mName;
        EntryType mEntryType;
        BaseObj mLoadedObj;

        /**
        * @return name of a renderscript object the index entry
        *         describes
        */
        public String getName() {
            return mName;
        }

        /**
        * @return type of a renderscript object the index entry
        *         describes
        */
        public EntryType getEntryType() {
            return mEntryType;
        }

        /**
        * @return renderscript object described by the entry
        */
        public BaseObj getObject() {
            mRS.validate();
            BaseObj obj = internalCreate(mRS, this);
            return obj;
        }

        /**
        * @return renderscript mesh object described by the entry
        */
        public Mesh getMesh() {
            return (Mesh)getObject();
        }

        static synchronized BaseObj internalCreate(RenderScript rs, IndexEntry entry) {
            if(entry.mLoadedObj != null) {
                return entry.mLoadedObj;
            }

            // to be purged on cleanup
            if(entry.mEntryType == EntryType.UNKNOWN) {
                return null;
            }

            int objectID = rs.nFileA3DGetEntryByIndex(entry.mID, entry.mIndex);
            if(objectID == 0) {
                return null;
            }

            switch (entry.mEntryType) {
            case MESH:
                entry.mLoadedObj = new Mesh(objectID, rs);
                break;
            }

            entry.mLoadedObj.updateFromNative();
            return entry.mLoadedObj;
        }

        IndexEntry(RenderScript rs, int index, int id, String name, EntryType type) {
            mRS = rs;
            mIndex = index;
            mID = id;
            mName = name;
            mEntryType = type;
            mLoadedObj = null;
        }
    }

    IndexEntry[] mFileEntries;
    InputStream mInputStream;

    FileA3D(int id, RenderScript rs, InputStream stream) {
        super(id, rs);
        mInputStream = stream;
    }

    private void initEntries() {
        int numFileEntries = mRS.nFileA3DGetNumIndexEntries(getID());
        if(numFileEntries <= 0) {
            return;
        }

        mFileEntries = new IndexEntry[numFileEntries];
        int[] ids = new int[numFileEntries];
        String[] names = new String[numFileEntries];

        mRS.nFileA3DGetIndexEntries(getID(), numFileEntries, ids, names);

        for(int i = 0; i < numFileEntries; i ++) {
            mFileEntries[i] = new IndexEntry(mRS, i, getID(), names[i], EntryType.toEntryType(ids[i]));
        }
    }

    /**
    * @return the numberof objects stored inside a FileA3D
    */
    public int getIndexEntryCount() {
        if(mFileEntries == null) {
            return 0;
        }
        return mFileEntries.length;
    }

    /**
    * Returns an index entry from the list of all objects inside
    * FileA3D
    *
    * @param index number of the entry from the list to return
    */
    public IndexEntry getIndexEntry(int index) {
        if(getIndexEntryCount() == 0 || index < 0 || index >= mFileEntries.length) {
            return null;
        }
        return mFileEntries[index];
    }

    /**
    * Creates a FileA3D object from an asset stored on disk
    *
    * @param rs Context to which the object will belong.
    * @param mgr asset manager used to load asset
    * @param path location of the file to load
    *
    */
    static public FileA3D createFromAsset(RenderScript rs, AssetManager mgr, String path) {
        rs.validate();
        int fileId = rs.nFileA3DCreateFromAsset(mgr, path);

        if(fileId == 0) {
            throw new RSRuntimeException("Unable to create a3d file from asset " + path);
        }
        FileA3D fa3d = new FileA3D(fileId, rs, null);
        fa3d.initEntries();
        return fa3d;
    }

    /**
    * Creates a FileA3D object from a file stored on disk
    *
    * @param rs Context to which the object will belong.
    * @param path location of the file to load
    *
    */
    static public FileA3D createFromFile(RenderScript rs, String path) {
        int fileId = rs.nFileA3DCreateFromFile(path);

        if(fileId == 0) {
            throw new RSRuntimeException("Unable to create a3d file from " + path);
        }
        FileA3D fa3d = new FileA3D(fileId, rs, null);
        fa3d.initEntries();
        return fa3d;
    }

    /**
    * Creates a FileA3D object from a file stored on disk
    *
    * @param rs Context to which the object will belong.
    * @param path location of the file to load
    *
    */
    static public FileA3D createFromFile(RenderScript rs, File path) {
        return createFromFile(rs, path.getAbsolutePath());
    }

    /**
    * Creates a FileA3D object from an application resource
    *
    * @param rs Context to which the object will belong.
    * @param res resource manager used for loading
    * @param id resource to create FileA3D from
    *
    */
    static public FileA3D createFromResource(RenderScript rs, Resources res, int id) {

        rs.validate();
        InputStream is = null;
        try {
            is = res.openRawResource(id);
        } catch (Exception e) {
            throw new RSRuntimeException("Unable to open resource " + id);
        }

        int fileId = 0;
        if (is instanceof AssetManager.AssetInputStream) {
            int asset = ((AssetManager.AssetInputStream) is).getAssetInt();
            fileId = rs.nFileA3DCreateFromAssetStream(asset);
        } else {
            throw new RSRuntimeException("Unsupported asset stream");
        }

        if(fileId == 0) {
            throw new RSRuntimeException("Unable to create a3d file from resource " + id);
        }
        FileA3D fa3d = new FileA3D(fileId, rs, is);
        fa3d.initEntries();
        return fa3d;

    }
}
