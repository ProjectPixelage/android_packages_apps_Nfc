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

import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.eq;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import android.annotation.NonNull;
import android.annotation.Nullable;
import android.app.NotificationManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.res.Resources;
import android.os.Handler;
import android.util.Log;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import com.android.dx.mockito.inline.extended.ExtendedMockito;

import org.junit.After;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mockito;
import org.mockito.MockitoSession;
import org.mockito.quality.Strictness;


@RunWith(AndroidJUnit4.class)
public class NfcTagAllowNotificationTest {

    private static final String TAG = NfcTagAllowNotificationTest.class.getSimpleName();
    private static final String NAME = "name";
    private static final String TITLE = "title";
    private MockitoSession mStaticMockSession;
    private Context mMockContext;
    private NfcTagAllowNotification mNfcTagAllowNotification;
    private NotificationManager mMockNotificationManager;

    @Before
    public void setUp() throws Exception {
        mStaticMockSession = ExtendedMockito.mockitoSession()
                .strictness(Strictness.LENIENT)
                .startMocking();
        mMockNotificationManager = Mockito.mock(NotificationManager.class);
        Resources mockResources = Mockito.mock(Resources.class);
        when(mockResources.getString(eq(R.string.nfc_tag_alert_title))).thenReturn(TITLE);

        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        mMockContext = new ContextWrapper(context) {
            @Override
            public Object getSystemService(String name) {
                if (Context.NOTIFICATION_SERVICE.equals(name)) {
                    Log.i(TAG, "[Mock] mMockNotificationManager");
                    return mMockNotificationManager;
                }
                return super.getSystemService(name);
            }

            @Override
            public Resources getResources() {
                Log.i(TAG, "[Mock] getResources");
                return mockResources;
            }

            @Override
            public Intent registerReceiverForAllUsers(@Nullable BroadcastReceiver receiver,
                    @NonNull IntentFilter filter, @Nullable String broadcastPermission,
                    @Nullable Handler scheduler) {
                Log.i(TAG, "[Mock] getIntent");
                return Mockito.mock(Intent.class);
            }
        };

        InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> mNfcTagAllowNotification = new NfcTagAllowNotification(mMockContext, NAME));
        Assert.assertNotNull(mNfcTagAllowNotification);
    }

    @After
    public void tearDown() throws Exception {
        mStaticMockSession.finishMocking();
    }

    @Test
    public void testStartNotification() {
        mNfcTagAllowNotification.startNotification();
        verify(mMockNotificationManager).createNotificationChannel(any());
    }
}
