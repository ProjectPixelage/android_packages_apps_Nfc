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

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.nfc.NfcAdapter;
import android.nfc.cardemulation.CardEmulation;
import android.os.Handler;
import android.util.Log;

import com.android.nfc.SharedPreferencesMigration;
import com.android.nfc.cardemulation.RegisteredServicesCacheMigration;

import java.lang.reflect.Method;

public class NfcMigrationBootCompleteReceiver extends BroadcastReceiver {
    private static final String TAG = "NfcMigrationBootCompletedReceiver";
    private static final String FEATURE_NFC_ANY = "android.hardware.nfc.any";

    private NfcAdapter mNfcAdapter;

    private void indicateDataMigration(boolean inProgress) {
        try {
            Method indicateDataMirationMethod =
                    NfcAdapter.class.getMethod("indicateDataMigration", boolean.class);
            indicateDataMirationMethod.invoke(mNfcAdapter, inProgress);
        } catch (Exception e) {
            Log.e(TAG, "Failed to indicate nfc data migration", e);
        }
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        Log.d(TAG, "onReceive: " + intent);
        if (Intent.ACTION_BOOT_COMPLETED.equals(action)) {
            PackageManager pm = context.getPackageManager();
            if (!pm.hasSystemFeature(FEATURE_NFC_ANY)) {
                pm.setApplicationEnabledSetting(context.getPackageName(),
                        PackageManager.COMPONENT_ENABLED_STATE_DISABLED, 0);
            } else {
                mNfcAdapter = NfcAdapter.getDefaultAdapter(context);
                if (mNfcAdapter == null) {
                    throw new IllegalStateException("Failed to get NFC adapter");
                }
                SharedPreferencesMigration preferencesMigration =
                        new SharedPreferencesMigration(context);
               if (!preferencesMigration.hasAlreadyMigrated()) {
                   Handler handler = new Handler();
                   // Let NFC stack handle user unlock before doing the migration.
                   handler.post(()  -> {
                       Log.d(TAG, "Starting NFC data Migration");
                       indicateDataMigration(true);
                       preferencesMigration.handleMigration();
                       RegisteredServicesCacheMigration cacheMigration =
                               new RegisteredServicesCacheMigration(context);
                       cacheMigration.handleMigration();
                       preferencesMigration.markMigrationComplete();
                       indicateDataMigration(false);
                       pm.setComponentEnabledSetting(new ComponentName(context, this.getClass()),
                           PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                           PackageManager.DONT_KILL_APP);
                   });
               }
            }
        }
    }
}
