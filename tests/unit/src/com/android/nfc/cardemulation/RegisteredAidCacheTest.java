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

package com.android.nfc.cardemulation;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.anyInt;
import static org.mockito.ArgumentMatchers.eq;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.verifyNoMoreInteractions;
import static org.mockito.Mockito.when;

import android.app.ActivityManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.pm.PackageManager;
import android.nfc.ComponentNameAndUser;
import android.nfc.Flags;
import android.nfc.cardemulation.ApduServiceInfo;
import android.nfc.cardemulation.CardEmulation;
import android.os.UserHandle;
import android.os.UserManager;

import androidx.test.ext.junit.runners.AndroidJUnit4;

import com.android.dx.mockito.inline.extended.ExtendedMockito;
import com.android.nfc.NfcService;

import org.junit.After;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.Captor;
import org.mockito.Mock;
import org.mockito.Mockito;
import org.mockito.MockitoSession;
import org.mockito.quality.Strictness;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;

@RunWith(AndroidJUnit4.class)
public class RegisteredAidCacheTest {

    private static final String PREFIX_AID = "ASDASD*";
    private static final String SUBSET_AID = "ASDASD#";
    private static final String EXACT_AID = "TASDASD";
    private static final String PAYMENT_AID_1 = "A000000004101012";
    private static final String PAYMENT_AID_2 = "A000000004101018";
    private static final String NON_PAYMENT_AID_1 = "F053414950454D";
    private static final String PREFIX_PAYMENT_AID = "A000000004*";
    private static final String NFC_FOREGROUND_PACKAGE_NAME = "com.android.test.foregroundnfc";
    private static final String NON_PAYMENT_NFC_PACKAGE_NAME = "com.android.test.nonpaymentnfc";
    private static final String WALLET_HOLDER_PACKAGE_NAME = "com.android.test.walletroleholder";
    private static final String WALLET_HOLDER_2_PACKAGE_NAME = "com.android.test.walletroleholder2";

    private static final ComponentName WALLET_PAYMENT_SERVICE =
            new ComponentName(
                    WALLET_HOLDER_PACKAGE_NAME,
                    "com.android.test.walletroleholder.WalletRoleHolderApduService");

    private static final ComponentName WALLET_PAYMENT_SERVICE_2 =
            new ComponentName(
                    WALLET_HOLDER_PACKAGE_NAME,
                    "com.android.test.walletroleholder.XWalletRoleHolderApduService");
    private static final ComponentName FOREGROUND_SERVICE =
            new ComponentName(
                    NFC_FOREGROUND_PACKAGE_NAME,
                    "com.android.test.foregroundnfc.ForegroundApduService");
    private static final ComponentName NON_PAYMENT_SERVICE =
            new ComponentName(
                    NON_PAYMENT_NFC_PACKAGE_NAME,
                    "com.android.test.nonpaymentnfc.NonPaymentApduService");

    private static final ComponentName PAYMENT_SERVICE =
            new ComponentName(
                    WALLET_HOLDER_2_PACKAGE_NAME,
                    "com.android.test.walletroleholder.WalletRoleHolderXApduService");

    private static final int USER_ID = 0;
    private static final UserHandle USER_HANDLE = UserHandle.of(USER_ID);

    @Mock private Context mContext;
    @Mock private WalletRoleObserver mWalletRoleObserver;
    @Mock private AidRoutingManager mAidRoutingManager;
    @Mock private UserManager mUserManager;
    @Mock private PackageManager mPackageManager;
    @Mock private NfcService mNfcService;

    @Captor
    private ArgumentCaptor<HashMap<String, AidRoutingManager.AidEntry>> mRoutingEntryMapCaptor;

    private MockitoSession mStaticMockSession;

    RegisteredAidCache mRegisteredAidCache;

    @Before
    public void setUp() {
        mStaticMockSession =
                ExtendedMockito.mockitoSession()
                        .mockStatic(ActivityManager.class)
                        .mockStatic(NfcService.class)
                        .mockStatic(Flags.class)
                        .strictness(Strictness.LENIENT)
                        .initMocks(this)
                        .startMocking();
        when(ActivityManager.getCurrentUser()).thenReturn(USER_ID);
        when(NfcService.getInstance()).thenReturn(mNfcService);
        when(mNfcService.getNciVersion()).thenReturn(NfcService.NCI_VERSION_1_0);
        when(mUserManager.getProfileParent(eq(USER_HANDLE))).thenReturn(USER_HANDLE);
        when(mContext.createContextAsUser(any(), anyInt())).thenReturn(mContext);
        when(mContext.getSystemService(eq(UserManager.class))).thenReturn(mUserManager);
        when (mContext.getPackageManager()).thenReturn(mPackageManager);
    }

    @After
    public void tearDown() {
        mStaticMockSession.finishMocking();
    }

    @Test
    public void testConstructor_supportsPrefixAndSubset() {
        supportPrefixAndSubset(true);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);

        verify(mAidRoutingManager).supportsAidPrefixRouting();
        verify(mAidRoutingManager).supportsAidSubsetRouting();
        assertTrue(mRegisteredAidCache.supportsAidPrefixRegistration());
        assertTrue(mRegisteredAidCache.supportsAidSubsetRegistration());
    }

    @Test
    public void testConstructor_doesNotSupportsPrefixAndSubset() {
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);

        verify(mAidRoutingManager).supportsAidPrefixRouting();
        verify(mAidRoutingManager).supportsAidSubsetRouting();
        assertFalse(mRegisteredAidCache.supportsAidPrefixRegistration());
        assertFalse(mRegisteredAidCache.supportsAidSubsetRegistration());
    }

    @Test
    public void testAidStaticMethods() {
        assertTrue(RegisteredAidCache.isPrefix(PREFIX_AID));
        assertTrue(RegisteredAidCache.isSubset(SUBSET_AID));
        assertTrue(RegisteredAidCache.isExact(EXACT_AID));

        assertFalse(RegisteredAidCache.isPrefix(EXACT_AID));
        assertFalse(RegisteredAidCache.isSubset(EXACT_AID));
        assertFalse(RegisteredAidCache.isExact(PREFIX_AID));
        assertFalse(RegisteredAidCache.isExact(SUBSET_AID));

        assertFalse(RegisteredAidCache.isPrefix(null));
        assertFalse(RegisteredAidCache.isSubset(null));
        assertFalse(RegisteredAidCache.isExact(null));
    }

    @Test
    public void testAidConflictResolution_walletRoleEnabledNfcDisabled_foregroundWins() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);
        mRegisteredAidCache.mNfcEnabled = false;

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1, NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT, CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        FOREGROUND_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1, NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT, CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        NON_PAYMENT_SERVICE,
                        true,
                        List.of(NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));

        mRegisteredAidCache.generateUserApduServiceInfoLocked(USER_ID, apduServiceInfos);
        mRegisteredAidCache.generateServiceMapLocked(apduServiceInfos);
        mRegisteredAidCache.onPreferredForegroundServiceChanged(
                new ComponentNameAndUser(USER_ID, FOREGROUND_SERVICE));
        RegisteredAidCache.AidResolveInfo resolveInfo =
                mRegisteredAidCache.resolveAid(PAYMENT_AID_1);

        verify(mAidRoutingManager).supportsAidPrefixRouting();
        verify(mAidRoutingManager).supportsAidSubsetRouting();
        assertEquals(FOREGROUND_SERVICE, resolveInfo.defaultService.getComponent());
        assertEquals(
                new ComponentNameAndUser(USER_ID, FOREGROUND_SERVICE),
                mRegisteredAidCache.getPreferredService());
        assertEquals(1, resolveInfo.services.size());
        assertEquals(CardEmulation.CATEGORY_PAYMENT, resolveInfo.category);
        verifyNoMoreInteractions(mAidRoutingManager);
    }

    @Test
    public void testAidConflictResolution_walletRoleEnabledNfcEnabled_walletWins() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);
        mRegisteredAidCache.mNfcEnabled = true;

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT),
                        false,
                        true,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT),
                        false,
                        true,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        NON_PAYMENT_SERVICE,
                        true,
                        List.of(NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));

        mRegisteredAidCache.generateUserApduServiceInfoLocked(USER_ID, apduServiceInfos);
        mRegisteredAidCache.generateServiceMapLocked(apduServiceInfos);
        mRegisteredAidCache.onWalletRoleHolderChanged(WALLET_HOLDER_PACKAGE_NAME, USER_ID);
        RegisteredAidCache.AidResolveInfo paymentResolveInfo =
                mRegisteredAidCache.resolveAid(PAYMENT_AID_1);
        RegisteredAidCache.AidResolveInfo nonPaymentResolveInfo =
                mRegisteredAidCache.resolveAid(NON_PAYMENT_AID_1);

        assertEquals(WALLET_PAYMENT_SERVICE, paymentResolveInfo.defaultService.getComponent());
        assertEquals(1, paymentResolveInfo.services.size());
        assertEquals(CardEmulation.CATEGORY_PAYMENT, paymentResolveInfo.category);
        assertEquals(NON_PAYMENT_SERVICE, nonPaymentResolveInfo.defaultService.getComponent());
        assertEquals(1, nonPaymentResolveInfo.services.size());
        assertEquals(CardEmulation.CATEGORY_OTHER, nonPaymentResolveInfo.category);
        verify(mAidRoutingManager).configureRouting(mRoutingEntryMapCaptor.capture(), eq(false));
        HashMap<String, AidRoutingManager.AidEntry> routingEntries =
                mRoutingEntryMapCaptor.getValue();
        assertTrue(routingEntries.containsKey(PAYMENT_AID_1));
        assertTrue(routingEntries.containsKey(NON_PAYMENT_AID_1));
        assertTrue(routingEntries.get(PAYMENT_AID_1).isOnHost);
        assertTrue(routingEntries.get(NON_PAYMENT_AID_1).isOnHost);
        assertNull(routingEntries.get(PAYMENT_AID_1).offHostSE);
        assertNull(routingEntries.get(NON_PAYMENT_AID_1).offHostSE);
        assertTrue(mRegisteredAidCache.isRequiresScreenOnServiceExist());
    }

    @Test
    public void testAidConflictResolution_walletRoleEnabledNfcEnabled_associatedRoleServices()
            throws PackageManager.NameNotFoundException {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        when(Flags.nfcAssociatedRoleServices()).thenReturn(true);
        when(mPackageManager.getProperty(
                eq(CardEmulation.PROPERTY_ALLOW_SHARED_ROLE_PRIORITY),
                eq(WALLET_HOLDER_PACKAGE_NAME)))
                .thenReturn(new PackageManager.Property(
                        CardEmulation.PROPERTY_ALLOW_SHARED_ROLE_PRIORITY,
                        true, WALLET_HOLDER_PACKAGE_NAME, null));

        mRegisteredAidCache = new RegisteredAidCache(mContext, mWalletRoleObserver,
                mAidRoutingManager);
        mRegisteredAidCache.mNfcEnabled = true;

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(createServiceInfoForAidRouting(
                WALLET_PAYMENT_SERVICE,
                true,
                List.of(PAYMENT_AID_1),
                List.of(CardEmulation.CATEGORY_PAYMENT),
                false,
                true,
                USER_ID,
                true,
                true));
        apduServiceInfos.add(createServiceInfoForAidRouting(
                PAYMENT_SERVICE,
                true,
                List.of(PAYMENT_AID_2),
                List.of(CardEmulation.CATEGORY_PAYMENT),
                false,
                true,
                USER_ID,
                true,
                true));

        mRegisteredAidCache.generateUserApduServiceInfoLocked(USER_ID, apduServiceInfos);
        mRegisteredAidCache.generateServiceMapLocked(apduServiceInfos);
        mRegisteredAidCache.onWalletRoleHolderChanged(WALLET_HOLDER_PACKAGE_NAME, USER_ID);

        mRegisteredAidCache.mAssociatedRoleServices = new HashSet<>(apduServiceInfos);
        mRegisteredAidCache.generateAidCacheLocked();

        RegisteredAidCache.AidResolveInfo paymentResolveInfo
                = mRegisteredAidCache.resolveAid(PAYMENT_AID_2);

        assertNotNull(paymentResolveInfo.defaultService);
        assertEquals(PAYMENT_SERVICE, paymentResolveInfo.defaultService.getComponent());
        assertEquals(CardEmulation.CATEGORY_PAYMENT, paymentResolveInfo.category);
        assertEquals(1, paymentResolveInfo.services.size());

        assertTrue(mRegisteredAidCache.isPreferredServicePackageNameForUser(
                WALLET_HOLDER_PACKAGE_NAME, USER_ID));
        assertTrue(mRegisteredAidCache.isPreferredServicePackageNameForUser(
                WALLET_HOLDER_2_PACKAGE_NAME, USER_ID));
    }

    @Test
    public void testAidConflictResolution_walletRoleEnabledNfcEnabledPreFixAid_walletWins() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(true);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);
        mRegisteredAidCache.mNfcEnabled = true;

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE,
                        true,
                        List.of(PREFIX_PAYMENT_AID),
                        List.of(CardEmulation.CATEGORY_PAYMENT),
                        false,
                        true,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT),
                        false,
                        true,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        NON_PAYMENT_SERVICE,
                        true,
                        List.of(NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));

        mRegisteredAidCache.generateUserApduServiceInfoLocked(USER_ID, apduServiceInfos);
        mRegisteredAidCache.generateServiceMapLocked(apduServiceInfos);
        mRegisteredAidCache.onWalletRoleHolderChanged(WALLET_HOLDER_PACKAGE_NAME, USER_ID);
        RegisteredAidCache.AidResolveInfo paymentResolveInfo =
                mRegisteredAidCache.resolveAid(PAYMENT_AID_1);
        RegisteredAidCache.AidResolveInfo nonPaymentResolveInfo =
                mRegisteredAidCache.resolveAid(NON_PAYMENT_AID_1);

        assertEquals(WALLET_PAYMENT_SERVICE, paymentResolveInfo.defaultService.getComponent());
        assertEquals(1, paymentResolveInfo.services.size());
        assertEquals(CardEmulation.CATEGORY_PAYMENT, paymentResolveInfo.category);
        assertEquals(NON_PAYMENT_SERVICE, nonPaymentResolveInfo.defaultService.getComponent());
        assertEquals(1, nonPaymentResolveInfo.services.size());
        assertEquals(CardEmulation.CATEGORY_OTHER, nonPaymentResolveInfo.category);
    }

    @Test
    public void testAidConflictResolution_walletRoleEnabled_twoServicesOnWallet_firstServiceWins() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1, NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT, CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE_2,
                        true,
                        List.of(PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT),
                        false,
                        false,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT),
                        false,
                        false,
                        USER_ID,
                        true));

        mRegisteredAidCache.generateUserApduServiceInfoLocked(USER_ID, apduServiceInfos);
        mRegisteredAidCache.generateServiceMapLocked(apduServiceInfos);
        mRegisteredAidCache.onWalletRoleHolderChanged(WALLET_HOLDER_PACKAGE_NAME, USER_ID);
        RegisteredAidCache.AidResolveInfo resolveInfo =
                mRegisteredAidCache.resolveAid(PAYMENT_AID_1);
        assertEquals(WALLET_PAYMENT_SERVICE, resolveInfo.defaultService.getComponent());
        assertEquals(2, resolveInfo.services.size());
        assertEquals(CardEmulation.CATEGORY_PAYMENT, resolveInfo.category);
    }

    @Test
    public void testAidConflictResolution_walletOtherServiceDisabled_nonDefaultServiceWins() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1, NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT, CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        false));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1, NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT, CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));

        mRegisteredAidCache.generateUserApduServiceInfoLocked(USER_ID, apduServiceInfos);
        mRegisteredAidCache.generateServiceMapLocked(apduServiceInfos);
        mRegisteredAidCache.onWalletRoleHolderChanged(WALLET_HOLDER_PACKAGE_NAME, USER_ID);
        RegisteredAidCache.AidResolveInfo resolveInfo =
                mRegisteredAidCache.resolveAid(NON_PAYMENT_AID_1);
        assertEquals(PAYMENT_SERVICE, resolveInfo.defaultService.getComponent());
        assertEquals(1, resolveInfo.services.size());
    }

    @Test
    public void testAidConflictResolution_walletOtherServiceDisabled_emptyServices() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1, NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT, CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        false));

        mRegisteredAidCache.generateUserApduServiceInfoLocked(USER_ID, apduServiceInfos);
        mRegisteredAidCache.generateServiceMapLocked(apduServiceInfos);
        mRegisteredAidCache.onWalletRoleHolderChanged(WALLET_HOLDER_PACKAGE_NAME, USER_ID);
        RegisteredAidCache.AidResolveInfo resolveInfo =
                mRegisteredAidCache.resolveAid(NON_PAYMENT_AID_1);
        assertNull(resolveInfo.defaultService);
        assertTrue(resolveInfo.services.isEmpty());
    }

    @Test
    public void testOnServicesUpdated_walletRoleEnabled() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);
        mRegisteredAidCache.mNfcEnabled = true;

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT),
                        false,
                        true,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT),
                        false,
                        true,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        NON_PAYMENT_SERVICE,
                        true,
                        List.of(NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_OTHER),
                        false,
                        true,
                        USER_ID,
                        true));

        mRegisteredAidCache.onServicesUpdated(USER_ID, apduServiceInfos);

        verify(mAidRoutingManager).supportsAidPrefixRouting();
        verify(mAidRoutingManager).supportsAidSubsetRouting();
        assertTrue(mRegisteredAidCache.mAidServices.containsKey(PAYMENT_AID_1));
        assertTrue(mRegisteredAidCache.mAidServices.containsKey(NON_PAYMENT_AID_1));
        assertEquals(2, mRegisteredAidCache.mAidServices.get(PAYMENT_AID_1).size());
        assertEquals(1, mRegisteredAidCache.mAidServices.get(NON_PAYMENT_AID_1).size());
        assertEquals(
                WALLET_PAYMENT_SERVICE,
                mRegisteredAidCache.mAidServices.get(PAYMENT_AID_1).get(0).service.getComponent());
        assertEquals(
                PAYMENT_SERVICE,
                mRegisteredAidCache.mAidServices.get(PAYMENT_AID_1).get(1).service.getComponent());
        verify(mAidRoutingManager).configureRouting(mRoutingEntryMapCaptor.capture(), eq(false));
        HashMap<String, AidRoutingManager.AidEntry> routingEntries =
                mRoutingEntryMapCaptor.getValue();
        assertTrue(routingEntries.containsKey(NON_PAYMENT_AID_1));
        assertTrue(routingEntries.get(NON_PAYMENT_AID_1).isOnHost);
        assertNull(routingEntries.get(NON_PAYMENT_AID_1).offHostSE);
        assertTrue(mRegisteredAidCache.isRequiresScreenOnServiceExist());
    }

    @Test
    public void testOnNfcEnabled() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT),
                        false,
                        false,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT),
                        false,
                        false,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        NON_PAYMENT_SERVICE,
                        true,
                        List.of(NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));

        mRegisteredAidCache.generateUserApduServiceInfoLocked(USER_ID, apduServiceInfos);
        mRegisteredAidCache.generateServiceMapLocked(apduServiceInfos);
        mRegisteredAidCache.onNfcEnabled();

        verify(mAidRoutingManager).supportsAidPrefixRouting();
        verify(mAidRoutingManager).supportsAidSubsetRouting();
        verify(mAidRoutingManager).configureRouting(mRoutingEntryMapCaptor.capture(), eq(false));
        assertFalse(mRegisteredAidCache.isRequiresScreenOnServiceExist());
    }

    @Test
    public void testOnNfcDisabled() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);
        mRegisteredAidCache.onNfcDisabled();

        verify(mAidRoutingManager).supportsAidPrefixRouting();
        verify(mAidRoutingManager).supportsAidSubsetRouting();
        verify(mAidRoutingManager).onNfccRoutingTableCleared();
    }

    @Test
    public void testPollingLoopFilterToForeground_walletRoleEnabled_walletSet() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);
        mRegisteredAidCache.mNfcEnabled = true;

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1, NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT, CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        FOREGROUND_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1, NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT, CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        NON_PAYMENT_SERVICE,
                        true,
                        List.of(NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));

        mRegisteredAidCache.onWalletRoleHolderChanged(WALLET_HOLDER_PACKAGE_NAME, USER_ID);
        mRegisteredAidCache.onPreferredForegroundServiceChanged(
                new ComponentNameAndUser(USER_ID, FOREGROUND_SERVICE));

        ApduServiceInfo resolvedApdu =
                mRegisteredAidCache.resolvePollingLoopFilterConflict(apduServiceInfos);

        assertEquals(resolvedApdu, apduServiceInfos.get(1));
    }

    @Test
    public void testPollingLoopFilterToWallet_walletRoleEnabled_walletSet() {
        setWalletRoleFlag(true);
        supportPrefixAndSubset(false);
        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);
        mRegisteredAidCache.mNfcEnabled = true;

        List<ApduServiceInfo> apduServiceInfos = new ArrayList<>();
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        WALLET_PAYMENT_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1, NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT, CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        FOREGROUND_SERVICE,
                        true,
                        List.of(PAYMENT_AID_1, NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_PAYMENT, CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));
        apduServiceInfos.add(
                createServiceInfoForAidRouting(
                        NON_PAYMENT_SERVICE,
                        true,
                        List.of(NON_PAYMENT_AID_1),
                        List.of(CardEmulation.CATEGORY_OTHER),
                        false,
                        false,
                        USER_ID,
                        true));

        mRegisteredAidCache.mDefaultWalletHolderPackageName = WALLET_HOLDER_PACKAGE_NAME;

        ApduServiceInfo resolvedApdu =
                mRegisteredAidCache.resolvePollingLoopFilterConflict(apduServiceInfos);

        assertEquals(resolvedApdu, apduServiceInfos.get(0));
    }

    private void setWalletRoleFlag(boolean flag) {
        when(mWalletRoleObserver.isWalletRoleFeatureEnabled()).thenReturn(flag);
    }

    private void supportPrefixAndSubset(boolean support) {
        when(mAidRoutingManager.supportsAidPrefixRouting()).thenReturn(support);
        when(mAidRoutingManager.supportsAidSubsetRouting()).thenReturn(support);
    }

    private static ApduServiceInfo createServiceInfoForAidRouting(
            ComponentName componentName,
            boolean onHost,
            List<String> aids,
            List<String> categories,
            boolean requiresUnlock,
            boolean requiresScreenOn,
            int uid,
            boolean isCategoryOtherServiceEnabled) {
        return createServiceInfoForAidRouting(componentName,
                onHost,
                aids,
                categories,
                requiresUnlock,
                requiresScreenOn,
                uid,
                isCategoryOtherServiceEnabled,
                false);
    }

    private static ApduServiceInfo createServiceInfoForAidRouting(
            ComponentName componentName,
            boolean onHost,
            List<String> aids,
            List<String> categories,
            boolean requiresUnlock,
            boolean requiresScreenOn,
            int uid,
            boolean isCategoryOtherServiceEnabled,
            boolean shareRolePriority) {
        ApduServiceInfo apduServiceInfo = Mockito.mock(ApduServiceInfo.class);
        when(apduServiceInfo.isOnHost()).thenReturn(onHost);
        when(apduServiceInfo.getAids()).thenReturn(aids);
        when(apduServiceInfo.getUid()).thenReturn(uid);
        when(apduServiceInfo.requiresUnlock()).thenReturn(requiresUnlock);
        when(apduServiceInfo.requiresScreenOn()).thenReturn(requiresScreenOn);
        when(apduServiceInfo.isCategoryOtherServiceEnabled())
                .thenReturn(isCategoryOtherServiceEnabled);
        when(apduServiceInfo.getComponent()).thenReturn(componentName);
        when(apduServiceInfo.shareRolePriority()).thenReturn(shareRolePriority);
        for (int i = 0; i < aids.size(); i++) {
            String aid = aids.get(i);
            String category = categories.get(i);
            when(apduServiceInfo.getCategoryForAid(eq(aid))).thenReturn(category);
        }
        return apduServiceInfo;
    }

    @Test
    public void testGetPreferredService() {

        mRegisteredAidCache =
                new RegisteredAidCache(mContext, mWalletRoleObserver, mAidRoutingManager);
        ComponentNameAndUser servicePair = mRegisteredAidCache.getPreferredService();
        Assert.assertNull(servicePair.getComponentName());
        mRegisteredAidCache.onPreferredForegroundServiceChanged(
                new ComponentNameAndUser(USER_ID, FOREGROUND_SERVICE));
        servicePair = mRegisteredAidCache.getPreferredService();
        Assert.assertNotNull(servicePair.getComponentName());
        assertEquals(new ComponentNameAndUser(USER_ID, FOREGROUND_SERVICE), servicePair);
    }
}
