/*
 * Copyright (C) 2024 The Android Open Source Project
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

package com.android.nfc;

import static android.content.Context.MODE_PRIVATE;

import android.app.ActivityManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.nfc.NfcAdapter;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.UserHandle;
import android.os.UserManager;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Used to migrate shared preferences files stored by
 * {@link com.android.nfc.NfcService} from AOSP stack to NFC mainline
 * module.
 */
public class SharedPreferencesMigration {
    private static final String TAG = "SharedPreferencesMigration";
    private static final String PREF = "NfcServicePrefs";
    public static final String PREF_TAG_APP_LIST = "TagIntentAppPreferenceListPrefs";
    private static final String PREF_NFC_ON = "nfc_on";
    private static final String PREF_SECURE_NFC_ON = "secure_nfc_on";
    private static final String PREF_NFC_READER_OPTION_ON = "nfc_reader_on";
    private static final String PREF_MIGRATION_TO_MAINLINE_COMPLETE = "migration_to_mainline_complete";

    private final SharedPreferences mSharedPreferences;
    private SharedPreferences mTagAppPrefListPreferences;
    private final Context mContext;
    private final NfcAdapter mNfcAdapter;

    public SharedPreferencesMigration(Context context) {
        mContext = context;
        mNfcAdapter = NfcAdapter.getDefaultAdapter(context);
        if (mNfcAdapter == null) {
            throw new IllegalStateException("Failed to get NFC adapter");
        }
        SharedPreferences sharedPreferences = context.getSharedPreferences(PREF, MODE_PRIVATE);
        SharedPreferences tagAppPrefListPreferences =
                context.getSharedPreferences(PREF_TAG_APP_LIST, MODE_PRIVATE);
        // Check both CE & DE directory for migration.
        if (sharedPreferences.getAll().isEmpty() && tagAppPrefListPreferences.getAll().isEmpty()) {
            Log.d(TAG, "Searching for NFC preferences in CE directory");
            Context ceContext = context.createCredentialProtectedStorageContext();
            sharedPreferences = ceContext.getSharedPreferences(PREF, MODE_PRIVATE);
            tagAppPrefListPreferences =
                    ceContext.getSharedPreferences(PREF_TAG_APP_LIST, MODE_PRIVATE);
        }
        mSharedPreferences = sharedPreferences;
        mTagAppPrefListPreferences = tagAppPrefListPreferences;
    }

    public boolean hasAlreadyMigrated() {
        return mSharedPreferences.getAll().isEmpty() ||
                mSharedPreferences.getBoolean(PREF_MIGRATION_TO_MAINLINE_COMPLETE, false);
    }

    public void markMigrationComplete() {
        mSharedPreferences.edit().putBoolean(PREF_MIGRATION_TO_MAINLINE_COMPLETE, true).apply();
    }

    private List<Integer> getEnabledUserIds() {
        List<Integer> userIds = new ArrayList<Integer>();
        UserManager um =
                mContext.createContextAsUser(UserHandle.of(ActivityManager.getCurrentUser()), 0)
                        .getSystemService(UserManager.class);
        List<UserHandle> luh = um.getEnabledProfiles();
        for (UserHandle uh : luh) {
            userIds.add(uh.getIdentifier());
        }
        return userIds;
    }

    private boolean setNfcEnabled(boolean enable) {
        try {
            if (mNfcAdapter.isEnabled() == enable) return true;
            CountDownLatch countDownLatch = new CountDownLatch(1);
            AtomicInteger state = new AtomicInteger(NfcAdapter.STATE_OFF);
            BroadcastReceiver nfcChangeListener = new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    int s = intent.getIntExtra(NfcAdapter.EXTRA_ADAPTER_STATE, NfcAdapter.STATE_OFF);
                    if (s == NfcAdapter.STATE_TURNING_ON || s == NfcAdapter.STATE_TURNING_OFF) {
                        return;
                    }
                    context.unregisterReceiver(this);
                    state.set(s);
                    countDownLatch.countDown();
                }
            };
            HandlerThread handlerThread = new HandlerThread("nfc_migration_state_listener");
            handlerThread.start();
            Handler handler = new Handler(handlerThread.getLooper());
            IntentFilter intentFilter = new IntentFilter();
            intentFilter.addAction(NfcAdapter.ACTION_ADAPTER_STATE_CHANGED);
            mContext.getApplicationContext().registerReceiver(
                    nfcChangeListener, intentFilter, null, handler);
            if (enable) {
                if (!mNfcAdapter.enable()) return false;
            } else {
                if (!mNfcAdapter.disable()) return false;
            }
            if (!countDownLatch.await(2000, TimeUnit.MILLISECONDS)) return false;
            return state.get() == (enable ? NfcAdapter.STATE_ON : NfcAdapter.STATE_OFF);
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        }
    }

    public void handleMigration() {
        Log.i(TAG, "Migrating preferences: " + mSharedPreferences.getAll()
                + ", " + mTagAppPrefListPreferences.getAll());
        if (mSharedPreferences.contains(PREF_NFC_ON)) {
            boolean enableNfc = mSharedPreferences.getBoolean(PREF_NFC_ON, false);
            Log.d(TAG, "enableNfc: " + enableNfc);
            if (!setNfcEnabled(enableNfc)) {
                Log.e(TAG, "Failed to set NFC " + (enableNfc ? "enabled" : "disabled"));
            }
        }
        if (mSharedPreferences.contains(PREF_SECURE_NFC_ON)) {
            boolean enableSecureNfc = mSharedPreferences.getBoolean(PREF_SECURE_NFC_ON, false);
            Log.d(TAG, "enableSecureNfc: " + enableSecureNfc);
            if (!mNfcAdapter.enableSecureNfc(enableSecureNfc)) {
                Log.e(TAG, "enableSecureNfc failed");
            }
        }
        if (mSharedPreferences.contains(PREF_NFC_READER_OPTION_ON)) {
            boolean enableReaderOption =
                mSharedPreferences.getBoolean(PREF_NFC_READER_OPTION_ON, false);
            Log.d(TAG, "enableSecureNfc: " + enableReaderOption);
            if (!mNfcAdapter.enableReaderOption(enableReaderOption)) {
                Log.e(TAG, "enableReaderOption failed");
            }
        }
        if (mTagAppPrefListPreferences != null) {
            try {
                for (Integer userId : getEnabledUserIds()) {
                    String jsonString =
                            mTagAppPrefListPreferences.getString(Integer.toString(userId),
                                    (new JSONObject()).toString());
                    if (jsonString != null) {
                        JSONObject jsonObject = new JSONObject(jsonString);
                        Iterator<String> keysItr = jsonObject.keys();
                        while (keysItr.hasNext()) {
                            String pkg = keysItr.next();
                            Boolean allow = jsonObject.getBoolean(pkg);
                            Log.d(TAG, "setTagIntentAppPreferenceForUser: " + pkg + " = " + allow);
                            if (mNfcAdapter.setTagIntentAppPreferenceForUser(userId, pkg, allow)
                                    != NfcAdapter.TAG_INTENT_APP_PREF_RESULT_SUCCESS) {
                                Log.e(TAG, "setTagIntentAppPreferenceForUser failed");
                            }
                        }
                    }
                }
            } catch (JSONException e) {
                Log.e(TAG, "JSONException: " + e);
            }
        }
    }

}
