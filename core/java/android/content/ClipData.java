/**
 * Copyright (c) 2010, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.content;

import android.content.res.AssetFileDescriptor;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;
import android.text.TextUtils;
import android.util.Log;

import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;

/**
 * Representation of a clipped data on the clipboard.
 *
 * <p>ClippedData is a complex type containing one or Item instances,
 * each of which can hold one or more representations of an item of data.
 * For display to the user, it also has a label and iconic representation.</p>
 *
 * <p>A ClipData contains a {@link ClipDescription}, which describes
 * important meta-data about the clip.  In particular, its
 * {@link ClipDescription#getMimeType(int) getDescription().getMimeType(int)}
 * must return correct MIME type(s) describing the data in the clip.  For help
 * in correctly constructing a clip with the correct MIME type, use
 * {@link #newPlainText(CharSequence, CharSequence)},
 * {@link #newUri(ContentResolver, CharSequence, Uri)}, and
 * {@link #newIntent(CharSequence, Intent)}.
 *
 * <p>Each Item instance can be one of three main classes of data: a simple
 * CharSequence of text, a single Intent object, or a Uri.  See {@link Item}
 * for more details.
 *
 * <a name="ImplementingPaste"></a>
 * <h3>Implementing Paste or Drop</h3>
 *
 * <p>To implement a paste or drop of a ClippedData object into an application,
 * the application must correctly interpret the data for its use.  If the {@link Item}
 * it contains is simple text or an Intent, there is little to be done: text
 * can only be interpreted as text, and an Intent will typically be used for
 * creating shortcuts (such as placing icons on the home screen) or other
 * actions.
 *
 * <p>If all you want is the textual representation of the clipped data, you
 * can use the convenience method {@link Item#coerceToText Item.coerceToText}.
 * In this case there is generally no need to worry about the MIME types
 * reported by {@link ClipDescription#getMimeType(int) getDescription().getMimeType(int)},
 * since any clip item an always be converted to a string.
 *
 * <p>More complicated exchanges will be done through URIs, in particular
 * "content:" URIs.  A content URI allows the recipient of a ClippedData item
 * to interact closely with the ContentProvider holding the data in order to
 * negotiate the transfer of that data.  The clip must also be filled in with
 * the available MIME types; {@link #newUri(ContentResolver, CharSequence, Uri)}
 * will take care of correctly doing this.
 *
 * <p>For example, here is the paste function of a simple NotePad application.
 * When retrieving the data from the clipboard, it can do either two things:
 * if the clipboard contains a URI reference to an existing note, it copies
 * the entire structure of the note into a new note; otherwise, it simply
 * coerces the clip into text and uses that as the new note's contents.
 *
 * {@sample development/samples/NotePad/src/com/example/android/notepad/NoteEditor.java
 *      paste}
 *
 * <p>In many cases an application can paste various types of streams of data.  For
 * example, an e-mail application may want to allow the user to paste an image
 * or other binary data as an attachment.  This is accomplished through the
 * ContentResolver {@link ContentResolver#getStreamTypes(Uri, String)} and
 * {@link ContentResolver#openTypedAssetFileDescriptor(Uri, String, android.os.Bundle)}
 * methods.  These allow a client to discover the type(s) of data that a particular
 * content URI can make available as a stream and retrieve the stream of data.
 *
 * <p>For example, the implementation of {@link Item#coerceToText Item.coerceToText}
 * itself uses this to try to retrieve a URI clip as a stream of text:
 *
 * {@sample frameworks/base/core/java/android/content/ClipData.java coerceToText}
 *
 * <a name="ImplementingCopy"></a>
 * <h3>Implementing Copy or Drag</h3>
 *
 * <p>To be the source of a clip, the application must construct a ClippedData
 * object that any recipient can interpret best for their context.  If the clip
 * is to contain a simple text, Intent, or URI, this is easy: an {@link Item}
 * containing the appropriate data type can be constructed and used.
 *
 * <p>More complicated data types require the implementation of support in
 * a ContentProvider for describing and generating the data for the recipient.
 * A common scenario is one where an application places on the clipboard the
 * content: URI of an object that the user has copied, with the data at that
 * URI consisting of a complicated structure that only other applications with
 * direct knowledge of the structure can use.
 *
 * <p>For applications that do not have intrinsic knowledge of the data structure,
 * the content provider holding it can make the data available as an arbitrary
 * number of types of data streams.  This is done by implementing the
 * ContentProvider {@link ContentProvider#getStreamTypes(Uri, String)} and
 * {@link ContentProvider#openTypedAssetFile(Uri, String, android.os.Bundle)}
 * methods.
 *
 * <p>Going back to our simple NotePad application, this is the implementation
 * it may have to convert a single note URI (consisting of a title and the note
 * text) into a stream of plain text data.
 *
 * {@sample development/samples/NotePad/src/com/example/android/notepad/NotePadProvider.java
 *      stream}
 *
 * <p>The copy operation in our NotePad application is now just a simple matter
 * of making a clip containing the URI of the note being copied:
 *
 * {@sample development/samples/NotePad/src/com/example/android/notepad/NotesList.java
 *      copy}
 *
 * <p>Note if a paste operation needs this clip as text (for example to paste
 * into an editor), then {@link Item#coerceToText(Context)} will ask the content
 * provider for the clip URI as text and successfully paste the entire note.
 */
public class ClipData implements Parcelable {
    static final String[] MIMETYPES_TEXT_PLAIN = new String[] {
        ClipDescription.MIMETYPE_TEXT_PLAIN };
    static final String[] MIMETYPES_TEXT_URILIST = new String[] {
        ClipDescription.MIMETYPE_TEXT_URILIST };
    static final String[] MIMETYPES_TEXT_INTENT = new String[] {
        ClipDescription.MIMETYPE_TEXT_INTENT };

    final ClipDescription mClipDescription;
    
    final Bitmap mIcon;

    final ArrayList<Item> mItems = new ArrayList<Item>();

    /**
     * Description of a single item in a ClippedData.
     *
     * <p>The types than an individual item can currently contain are:</p>
     *
     * <ul>
     * <li> Text: a basic string of text.  This is actually a CharSequence,
     * so it can be formatted text supported by corresponding Android built-in
     * style spans.  (Custom application spans are not supported and will be
     * stripped when transporting through the clipboard.)
     * <li> Intent: an arbitrary Intent object.  A typical use is the shortcut
     * to create when pasting a clipped item on to the home screen.
     * <li> Uri: a URI reference.  This may be any URI (such as an http: URI
     * representing a bookmark), however it is often a content: URI.  Using
     * content provider references as clips like this allows an application to
     * share complex or large clips through the standard content provider
     * facilities.
     * </ul>
     */
    public static class Item {
        final CharSequence mText;
        final Intent mIntent;
        final Uri mUri;

        /**
         * Create an Item consisting of a single block of (possibly styled) text.
         */
        public Item(CharSequence text) {
            mText = text;
            mIntent = null;
            mUri = null;
        }

        /**
         * Create an Item consisting of an arbitrary Intent.
         */
        public Item(Intent intent) {
            mText = null;
            mIntent = intent;
            mUri = null;
        }

        /**
         * Create an Item consisting of an arbitrary URI.
         */
        public Item(Uri uri) {
            mText = null;
            mIntent = null;
            mUri = uri;
        }

        /**
         * Create a complex Item, containing multiple representations of
         * text, intent, and/or URI.
         */
        public Item(CharSequence text, Intent intent, Uri uri) {
            mText = text;
            mIntent = intent;
            mUri = uri;
        }

        /**
         * Retrieve the raw text contained in this Item.
         */
        public CharSequence getText() {
            return mText;
        }

        /**
         * Retrieve the raw Intent contained in this Item.
         */
        public Intent getIntent() {
            return mIntent;
        }

        /**
         * Retrieve the raw URI contained in this Item.
         */
        public Uri getUri() {
            return mUri;
        }

        /**
         * Turn this item into text, regardless of the type of data it
         * actually contains.
         *
         * <p>The algorithm for deciding what text to return is:
         * <ul>
         * <li> If {@link #getText} is non-null, return that.
         * <li> If {@link #getUri} is non-null, try to retrieve its data
         * as a text stream from its content provider.  If this succeeds, copy
         * the text into a String and return it.  If it is not a content: URI or
         * the content provider does not supply a text representation, return
         * the raw URI as a string.
         * <li> If {@link #getIntent} is non-null, convert that to an intent:
         * URI and returnit.
         * <li> Otherwise, return an empty string.
         * </ul>
         *
         * @param context The caller's Context, from which its ContentResolver
         * and other things can be retrieved.
         * @return Returns the item's textual representation.
         */
//BEGIN_INCLUDE(coerceToText)
        public CharSequence coerceToText(Context context) {
            // If this Item has an explicit textual value, simply return that.
            if (mText != null) {
                return mText;
            }

            // If this Item has a URI value, try using that.
            if (mUri != null) {

                // First see if the URI can be opened as a plain text stream
                // (of any sub-type).  If so, this is the best textual
                // representation for it.
                FileInputStream stream = null;
                try {
                    // Ask for a stream of the desired type.
                    AssetFileDescriptor descr = context.getContentResolver()
                            .openTypedAssetFileDescriptor(mUri, "text/*", null);
                    stream = descr.createInputStream();
                    InputStreamReader reader = new InputStreamReader(stream, "UTF-8");

                    // Got it...  copy the stream into a local string and return it.
                    StringBuilder builder = new StringBuilder(128);
                    char[] buffer = new char[8192];
                    int len;
                    while ((len=reader.read(buffer)) > 0) {
                        builder.append(buffer, 0, len);
                    }
                    return builder.toString();

                } catch (FileNotFoundException e) {
                    // Unable to open content URI as text...  not really an
                    // error, just something to ignore.

                } catch (IOException e) {
                    // Something bad has happened.
                    Log.w("ClippedData", "Failure loading text", e);
                    return e.toString();

                } finally {
                    if (stream != null) {
                        try {
                            stream.close();
                        } catch (IOException e) {
                        }
                    }
                }

                // If we couldn't open the URI as a stream, then the URI itself
                // probably serves fairly well as a textual representation.
                return mUri.toString();
            }

            // Finally, if all we have is an Intent, then we can just turn that
            // into text.  Not the most user-friendly thing, but it's something.
            if (mIntent != null) {
                return mIntent.toUri(Intent.URI_INTENT_SCHEME);
            }

            // Shouldn't get here, but just in case...
            return "";
        }
//END_INCLUDE(coerceToText)
    }

    /**
     * Create a new clip.
     *
     * @param label Label to show to the user describing this clip.
     * @param mimeTypes An array of MIME types this data is available as.
     * @param item The contents of the first item in the clip.
     */
    public ClipData(CharSequence label, String[] mimeTypes, Item item) {
        mClipDescription = new ClipDescription(label, mimeTypes);
        if (item == null) {
            throw new NullPointerException("item is null");
        }
        mIcon = null;
        mItems.add(item);
    }

    /**
     * Create a new clip.
     *
     * @param description The ClipDescription describing the clip contents.
     * @param item The contents of the first item in the clip.
     */
    public ClipData(ClipDescription description, Item item) {
        mClipDescription = description;
        if (item == null) {
            throw new NullPointerException("item is null");
        }
        mIcon = null;
        mItems.add(item);
    }

    /**
     * Create a new ClipData holding data of the type
     * {@link ClipDescription#MIMETYPE_TEXT_PLAIN}.
     *
     * @param label User-visible label for the clip data.
     * @param text The actual text in the clip.
     * @return Returns a new ClipData containing the specified data.
     */
    static public ClipData newPlainText(CharSequence label, CharSequence text) {
        Item item = new Item(text);
        return new ClipData(label, MIMETYPES_TEXT_PLAIN, item);
    }

    @Deprecated
    static public ClipData newPlainText(CharSequence label, Bitmap icon, CharSequence text) {
        return newPlainText(label, text);
    }

    /**
     * Create a new ClipData holding an Intent with MIME type
     * {@link ClipDescription#MIMETYPE_TEXT_INTENT}.
     *
     * @param label User-visible label for the clip data.
     * @param intent The actual Intent in the clip.
     * @return Returns a new ClipData containing the specified data.
     */
    static public ClipData newIntent(CharSequence label, Intent intent) {
        Item item = new Item(intent);
        return new ClipData(label, MIMETYPES_TEXT_INTENT, item);
    }

    @Deprecated
    static public ClipData newIntent(CharSequence label, Bitmap icon, Intent intent) {
        return newIntent(label, intent);
    }
    
    /**
     * Create a new ClipData holding a URI.  If the URI is a content: URI,
     * this will query the content provider for the MIME type of its data and
     * use that as the MIME type.  Otherwise, it will use the MIME type
     * {@link ClipDescription#MIMETYPE_TEXT_URILIST}.
     *
     * @param resolver ContentResolver used to get information about the URI.
     * @param label User-visible label for the clip data.
     * @param uri The URI in the clip.
     * @return Returns a new ClipData containing the specified data.
     */
    static public ClipData newUri(ContentResolver resolver, CharSequence label,
            Uri uri) {
        Item item = new Item(uri);
        String[] mimeTypes = null;
        if ("content".equals(uri.getScheme())) {
            String realType = resolver.getType(uri);
            mimeTypes = resolver.getStreamTypes(uri, "*/*");
            if (mimeTypes == null) {
                if (realType != null) {
                    mimeTypes = new String[] { realType, ClipDescription.MIMETYPE_TEXT_URILIST };
                }
            } else {
                String[] tmp = new String[mimeTypes.length + (realType != null ? 2 : 1)];
                int i = 0;
                if (realType != null) {
                    tmp[0] = realType;
                    i++;
                }
                System.arraycopy(mimeTypes, 0, tmp, i, mimeTypes.length);
                tmp[i + mimeTypes.length] = ClipDescription.MIMETYPE_TEXT_URILIST;
                mimeTypes = tmp;
            }
        }
        if (mimeTypes == null) {
            mimeTypes = MIMETYPES_TEXT_URILIST;
        }
        return new ClipData(label, mimeTypes, item);
    }

    @Deprecated
    static public ClipData newUri(ContentResolver resolver, CharSequence label,
            Bitmap icon, Uri uri) {
        return newUri(resolver, label, uri);
    }
    
    /**
     * Create a new ClipData holding an URI with MIME type
     * {@link ClipDescription#MIMETYPE_TEXT_URILIST}.
     * Unlike {@link #newUri(ContentResolver, CharSequence, Uri)}, nothing
     * is inferred about the URI -- if it is a content: URI holding a bitmap,
     * the reported type will still be uri-list.  Use this with care!
     *
     * @param label User-visible label for the clip data.
     * @param uri The URI in the clip.
     * @return Returns a new ClipData containing the specified data.
     */
    static public ClipData newRawUri(CharSequence label, Uri uri) {
        Item item = new Item(uri);
        return new ClipData(label, MIMETYPES_TEXT_URILIST, item);
    }

    @Deprecated
    static public ClipData newRawUri(CharSequence label, Bitmap icon, Uri uri) {
        return newRawUri(label, uri);
    }
    
    /**
     * Return the {@link ClipDescription} associated with this data, describing
     * what it contains.
     */
    public ClipDescription getDescription() {
        return mClipDescription;
    }
    
    /**
     * Add a new Item to the overall ClipData container.
     */
    public void addItem(Item item) {
        if (item == null) {
            throw new NullPointerException("item is null");
        }
        mItems.add(item);
    }

    /** @hide */
    public Bitmap getIcon() {
        return mIcon;
    }

    /**
     * Return the number of items in the clip data.
     */
    public int getItemCount() {
        return mItems.size();
    }

    /**
     * Return a single item inside of the clip data.  The index can range
     * from 0 to {@link #getItemCount()}-1.
     */
    public Item getItemAt(int index) {
        return mItems.get(index);
    }

    @Deprecated
    public Item getItem(int index) {
        return getItemAt(index);
    }
    
    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        mClipDescription.writeToParcel(dest, flags);
        if (mIcon != null) {
            dest.writeInt(1);
            mIcon.writeToParcel(dest, flags);
        } else {
            dest.writeInt(0);
        }
        final int N = mItems.size();
        dest.writeInt(N);
        for (int i=0; i<N; i++) {
            Item item = mItems.get(i);
            TextUtils.writeToParcel(item.mText, dest, flags);
            if (item.mIntent != null) {
                dest.writeInt(1);
                item.mIntent.writeToParcel(dest, flags);
            } else {
                dest.writeInt(0);
            }
            if (item.mUri != null) {
                dest.writeInt(1);
                item.mUri.writeToParcel(dest, flags);
            } else {
                dest.writeInt(0);
            }
        }
    }

    ClipData(Parcel in) {
        mClipDescription = new ClipDescription(in);
        if (in.readInt() != 0) {
            mIcon = Bitmap.CREATOR.createFromParcel(in);
        } else {
            mIcon = null;
        }
        final int N = in.readInt();
        for (int i=0; i<N; i++) {
            CharSequence text = TextUtils.CHAR_SEQUENCE_CREATOR.createFromParcel(in);
            Intent intent = in.readInt() != 0 ? Intent.CREATOR.createFromParcel(in) : null;
            Uri uri = in.readInt() != 0 ? Uri.CREATOR.createFromParcel(in) : null;
            mItems.add(new Item(text, intent, uri));
        }
    }

    public static final Parcelable.Creator<ClipData> CREATOR =
        new Parcelable.Creator<ClipData>() {

            public ClipData createFromParcel(Parcel source) {
                return new ClipData(source);
            }

            public ClipData[] newArray(int size) {
                return new ClipData[size];
            }
        };
}
