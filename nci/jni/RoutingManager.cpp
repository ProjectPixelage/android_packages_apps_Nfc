/*
 * Copyright (C) 2013 The Android Open Source Project
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

/*
 *  Manage the listen-mode routing table.
 */

#include "RoutingManager.h"
// Redefined by android-base headers.
#undef ATTRIBUTE_UNUSED

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedLocalRef.h>

#include "JavaClassConstants.h"
#include "nfa_ce_api.h"
#include "nfa_ee_api.h"
#include "nfc_config.h"

using android::base::StringPrintf;

extern bool gActivated;
extern SyncEvent gDeactivatedEvent;

const JNINativeMethod RoutingManager::sMethods[] = {
    {"doGetDefaultRouteDestination", "()I",
     (void*)RoutingManager::
         com_android_nfc_cardemulation_doGetDefaultRouteDestination},
    {"doGetDefaultOffHostRouteDestination", "()I",
     (void*)RoutingManager::
         com_android_nfc_cardemulation_doGetDefaultOffHostRouteDestination},
    {"doGetDefaultFelicaRouteDestination", "()I",
     (void*)RoutingManager::
         com_android_nfc_cardemulation_doGetDefaultFelicaRouteDestination},
    {"doGetOffHostUiccDestination", "()[B",
     (void*)RoutingManager::
         com_android_nfc_cardemulation_doGetOffHostUiccDestination},
    {"doGetOffHostEseDestination", "()[B",
     (void*)RoutingManager::
         com_android_nfc_cardemulation_doGetOffHostEseDestination},
    {"doGetAidMatchingMode", "()I",
     (void*)RoutingManager::com_android_nfc_cardemulation_doGetAidMatchingMode},
    {"doGetDefaultIsoDepRouteDestination", "()I",
     (void*)RoutingManager::
         com_android_nfc_cardemulation_doGetDefaultIsoDepRouteDestination},
    {"doGetDefaultScRouteDestination", "()I",
     (void*)RoutingManager::
         com_android_nfc_cardemulation_doGetDefaultScRouteDestination}};

static const int MAX_NUM_EE = 5;
// SCBR from host works only when App is in foreground
static const uint8_t SYS_CODE_PWR_STATE_HOST = 0x01;
static const uint16_t DEFAULT_SYS_CODE = 0xFEFE;

static const uint8_t AID_ROUTE_QUAL_PREFIX = 0x10;

RoutingManager::RoutingManager()
    : mSecureNfcEnabled(false),
      mNativeData(NULL),
      mAidRoutingConfigured(false) {
  static const char fn[] = "RoutingManager::RoutingManager()";

  mDefaultOffHostRoute =
      NfcConfig::getUnsigned(NAME_DEFAULT_OFFHOST_ROUTE, 0x00);

  if (NfcConfig::hasKey(NAME_OFFHOST_ROUTE_UICC)) {
    mOffHostRouteUicc = NfcConfig::getBytes(NAME_OFFHOST_ROUTE_UICC);
  }

  if (NfcConfig::hasKey(NAME_OFFHOST_ROUTE_ESE)) {
    mOffHostRouteEse = NfcConfig::getBytes(NAME_OFFHOST_ROUTE_ESE);
  }

  mDefaultFelicaRoute = NfcConfig::getUnsigned(NAME_DEFAULT_NFCF_ROUTE, 0x00);
  LOG(DEBUG) << StringPrintf("%s: Active SE for Nfc-F is 0x%02X", fn,
                             mDefaultFelicaRoute);

  mDefaultEe = NfcConfig::getUnsigned(NAME_DEFAULT_ROUTE, 0x00);
  LOG(DEBUG) << StringPrintf("%s: default route is 0x%02X", fn, mDefaultEe);

  mAidMatchingMode =
      NfcConfig::getUnsigned(NAME_AID_MATCHING_MODE, AID_MATCHING_EXACT_ONLY);

  mDefaultSysCodeRoute =
      NfcConfig::getUnsigned(NAME_DEFAULT_SYS_CODE_ROUTE, 0xC0);

  mDefaultSysCodePowerstate =
      NfcConfig::getUnsigned(NAME_DEFAULT_SYS_CODE_PWR_STATE, 0x19);

  mDefaultSysCode = DEFAULT_SYS_CODE;
  if (NfcConfig::hasKey(NAME_DEFAULT_SYS_CODE)) {
    std::vector<uint8_t> pSysCode = NfcConfig::getBytes(NAME_DEFAULT_SYS_CODE);
    if (pSysCode.size() == 0x02) {
      mDefaultSysCode = ((pSysCode[0] << 8) | ((int)pSysCode[1] << 0));
      LOG(DEBUG) << StringPrintf("%s: DEFAULT_SYS_CODE: 0x%02X", __func__,
                                 mDefaultSysCode);
    }
  }

  mOffHostAidRoutingPowerState =
      NfcConfig::getUnsigned(NAME_OFFHOST_AID_ROUTE_PWR_STATE, 0x01);

  mDefaultIsoDepRoute = NfcConfig::getUnsigned(NAME_DEFAULT_ISODEP_ROUTE, 0x0);

  mHostListenTechMask =
      NfcConfig::getUnsigned(NAME_HOST_LISTEN_TECH_MASK,
                             NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_F);

  mOffHostListenTechMask = NfcConfig::getUnsigned(
      NAME_OFFHOST_LISTEN_TECH_MASK,
      NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B | NFA_TECHNOLOGY_MASK_F);

  memset(&mEeInfo, 0, sizeof(mEeInfo));
  mReceivedEeInfo = false;
  mSeTechMask = 0x00;
  mIsScbrSupported = false;

  mNfcFOnDhHandle = NFA_HANDLE_INVALID;

  mDeinitializing = false;
  mEeInfoChanged = false;
}

RoutingManager::~RoutingManager() {}

bool RoutingManager::initialize(nfc_jni_native_data* native) {
  static const char fn[] = "RoutingManager::initialize()";
  mNativeData = native;
  mRxDataBuffer.clear();

  {
    SyncEventGuard guard(mEeRegisterEvent);
    LOG(DEBUG) << fn << ": try ee register";
    tNFA_STATUS nfaStat = NFA_EeRegister(nfaEeCallback);
    if (nfaStat != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf("%s: fail ee register; error=0x%X", fn,
                                 nfaStat);
      return false;
    }
    mEeRegisterEvent.wait();
  }

  if ((mDefaultOffHostRoute != 0) || (mDefaultFelicaRoute != 0)) {
    // Wait for EE info if needed
    SyncEventGuard guard(mEeInfoEvent);
    if (!mReceivedEeInfo) {
      LOG(INFO) << fn << "Waiting for EE info";
      mEeInfoEvent.wait();
    }
  }
  mSeTechMask = updateEeTechRouteSetting();

  // Set the host-routing Tech
  tNFA_STATUS nfaStat = NFA_CeSetIsoDepListenTech(
      mHostListenTechMask & (NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B));

  if (nfaStat != NFA_STATUS_OK)
    LOG(ERROR) << StringPrintf("Failed to configure CE IsoDep technologies");

  // Register a wild-card for AIDs routed to the host
  nfaStat = NFA_CeRegisterAidOnDH(NULL, 0, stackCallback);
  if (nfaStat != NFA_STATUS_OK)
    LOG(ERROR) << fn << "Failed to register wildcard AID for DH";

  updateDefaultRoute();
  updateDefaultProtocolRoute();

  // For startup case with NFC secure enabled.
  if (mSecureNfcEnabled) {
    NFA_SetNfcSecure(mSecureNfcEnabled);
  }

  return true;
}

RoutingManager& RoutingManager::getInstance() {
  static RoutingManager manager;
  return manager;
}

void RoutingManager::enableRoutingToHost() {
  static const char fn[] = "RoutingManager::enableRoutingToHost()";
  tNFA_STATUS nfaStat;
  SyncEventGuard guard(mRoutingEvent);

  // Default routing for T3T protocol
  if (!mIsScbrSupported && mDefaultEe == NFC_DH_ID) {
    nfaStat = NFA_EeSetDefaultProtoRouting(NFC_DH_ID, NFA_PROTOCOL_MASK_T3T, 0,
                                           0, 0, 0, 0);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default proto routing for T3T";
  }

  // Default routing for IsoDep protocol
  tNFA_PROTOCOL_MASK protoMask = NFA_PROTOCOL_MASK_ISO_DEP;
  if (mDefaultIsoDepRoute == NFC_DH_ID) {
    nfaStat = NFA_EeSetDefaultProtoRouting(
        NFC_DH_ID, protoMask, 0, 0, mSecureNfcEnabled ? 0 : protoMask, 0, 0);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default proto routing for IsoDep";
  }

  // Route Nfc-A to host if we don't have a SE
  tNFA_TECHNOLOGY_MASK techMask = NFA_TECHNOLOGY_MASK_A;
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_A) &&
      (mSeTechMask & NFA_TECHNOLOGY_MASK_A) == 0) {
    nfaStat = NFA_EeSetDefaultTechRouting(
        NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask,
        mSecureNfcEnabled ? 0 : techMask, mSecureNfcEnabled ? 0 : techMask);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-A";
  }

  // Route Nfc-B to host if we don't have a SE
  techMask = NFA_TECHNOLOGY_MASK_B;
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_B) &&
      (mSeTechMask & NFA_TECHNOLOGY_MASK_B) == 0) {
    nfaStat = NFA_EeSetDefaultTechRouting(
        NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask,
        mSecureNfcEnabled ? 0 : techMask, mSecureNfcEnabled ? 0 : techMask);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-B";
  }

  // Route Nfc-F to host if we don't have a SE
  techMask = NFA_TECHNOLOGY_MASK_F;
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_F) &&
      (mSeTechMask & NFA_TECHNOLOGY_MASK_F) == 0) {
    nfaStat = NFA_EeSetDefaultTechRouting(
        NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask,
        mSecureNfcEnabled ? 0 : techMask, mSecureNfcEnabled ? 0 : techMask);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-F";
  }
}

void RoutingManager::disableRoutingToHost() {
  static const char fn[] = "RoutingManager::disableRoutingToHost()";
  tNFA_STATUS nfaStat;
  SyncEventGuard guard(mRoutingEvent);

  // Clear default routing for IsoDep protocol
  if (mDefaultIsoDepRoute == NFC_DH_ID) {
    nfaStat =
        NFA_EeClearDefaultProtoRouting(NFC_DH_ID, NFA_PROTOCOL_MASK_ISO_DEP);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to clear default proto routing for IsoDep";
  }

  // Clear default routing for Nfc-A technology if we don't have a SE
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_A) &&
      (mSeTechMask & NFA_TECHNOLOGY_MASK_A) == 0) {
    nfaStat = NFA_EeClearDefaultTechRouting(NFC_DH_ID, NFA_TECHNOLOGY_MASK_A);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to clear default tech routing for Nfc-A";
  }

  // Clear default routing for Nfc-B technology if we don't have a SE
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_B) &&
      (mSeTechMask & NFA_TECHNOLOGY_MASK_B) == 0) {
    nfaStat = NFA_EeClearDefaultTechRouting(NFC_DH_ID, NFA_TECHNOLOGY_MASK_B);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to clear default tech routing for Nfc-B";
  }

  // Clear default routing for Nfc-F technology if we don't have a SE
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_F) &&
      (mSeTechMask & NFA_TECHNOLOGY_MASK_F) == 0) {
    nfaStat = NFA_EeClearDefaultTechRouting(NFC_DH_ID, NFA_TECHNOLOGY_MASK_F);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to clear default tech routing for Nfc-F";
  }

  // Clear default routing for T3T protocol
  if (!mIsScbrSupported && mDefaultEe == NFC_DH_ID) {
    nfaStat = NFA_EeClearDefaultProtoRouting(NFC_DH_ID, NFA_PROTOCOL_MASK_T3T);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to clear default proto routing for T3T";
  }
}

/*******************************************************************************
 **
 ** Function:        isTypeATypeBTechSupportedInEe
 **
 ** Description:     receive eeHandle
 **
 ** Returns:         true  : if EE support protocol type A/B
 **                  false : if EE doesn't protocol type A/B
 **
 *******************************************************************************/
bool RoutingManager::isTypeATypeBTechSupportedInEe(tNFA_HANDLE eeHandle) {
  static const char fn[] = "RoutingManager::isTypeATypeBTechSupportedInEe";
  bool status = false;
  uint8_t mActualNumEe = MAX_NUM_EE;
  tNFA_EE_INFO eeInfo[mActualNumEe];
  memset(&eeInfo, 0, mActualNumEe * sizeof(tNFA_EE_INFO));
  tNFA_STATUS nfaStat = NFA_EeGetInfo(&mActualNumEe, eeInfo);
  LOG(DEBUG) << fn;
  if (nfaStat != NFA_STATUS_OK) {
    return status;
  }
  for (auto i = 0; i < mActualNumEe; i++) {
    if (eeHandle == eeInfo[i].ee_handle) {
      if (eeInfo[i].la_protocol || eeInfo[i].lb_protocol) {
        status = true;
        break;
      }
    }
  }
  return status;
}

bool RoutingManager::addAidRouting(const uint8_t* aid, uint8_t aidLen,
                                   int route, int aidInfo, int power) {
  static const char fn[] = "RoutingManager::addAidRouting";
  LOG(DEBUG) << fn << ": enter";
  uint8_t powerState = 0x01;
  if (!mSecureNfcEnabled) {
    if (power == 0x00) {
      powerState = (route != 0x00) ? mOffHostAidRoutingPowerState : 0x11;
    } else {
      powerState =
          (route != 0x00) ? mOffHostAidRoutingPowerState & power : power;
    }
  }
  SyncEventGuard guard(mAidAddRemoveEvent);
  mAidRoutingConfigured = false;
  tNFA_STATUS nfaStat =
      NFA_EeAddAidRouting(route, aidLen, (uint8_t*)aid, powerState, aidInfo);
  if (nfaStat == NFA_STATUS_OK) {
    mAidAddRemoveEvent.wait();
  }
  if (mAidRoutingConfigured) {
    LOG(DEBUG) << fn << ": routed AID";
    return true;
  } else {
    LOG(ERROR) << fn << ": failed to route AID";
    return false;
  }
}

bool RoutingManager::removeAidRouting(const uint8_t* aid, uint8_t aidLen) {
  static const char fn[] = "RoutingManager::removeAidRouting";
  LOG(DEBUG) << fn << ": enter";

  if (aidLen != 0) {
    LOG(DEBUG) << StringPrintf("%s : len=%d, 0x%x 0x%x 0x%x 0x%x 0x%x", fn,
                               aidLen, *(aid), *(aid + 1), *(aid + 2),
                               *(aid + 3), *(aid + 4));
  } else {
    LOG(DEBUG) << fn << "Remove Empty aid";
  }

  SyncEventGuard guard(mAidAddRemoveEvent);
  mAidRoutingConfigured = false;
  tNFA_STATUS nfaStat = NFA_EeRemoveAidRouting(aidLen, (uint8_t*)aid);
  if (nfaStat == NFA_STATUS_OK) {
    mAidAddRemoveEvent.wait();
  }
  if (mAidRoutingConfigured) {
    LOG(DEBUG) << fn << ": removed AID";
    return true;
  } else {
    LOG(WARNING) << fn << ": failed to remove AID";
    return false;
  }
}

tNFA_STATUS RoutingManager::commitRouting() {
  static const char fn[] = "RoutingManager::commitRouting";
  tNFA_STATUS nfaStat = 0;
  LOG(DEBUG) << fn;
  if(mEeInfoChanged) {
    mSeTechMask = updateEeTechRouteSetting();
    mEeInfoChanged = false;
  }
  {
    SyncEventGuard guard(mEeUpdateEvent);
    nfaStat = NFA_EeUpdateNow();
    if (nfaStat == NFA_STATUS_OK) {
      mEeUpdateEvent.wait();  // wait for NFA_EE_UPDATED_EVT
    }
  }
  return nfaStat;
}

void RoutingManager::onNfccShutdown() {
  static const char fn[] = "RoutingManager:onNfccShutdown";
  if (mDefaultOffHostRoute == 0x00 && mDefaultFelicaRoute == 0x00) return;

  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  uint8_t actualNumEe = MAX_NUM_EE;
  tNFA_EE_INFO eeInfo[MAX_NUM_EE];
  mDeinitializing = true;

  memset(&eeInfo, 0, sizeof(eeInfo));
  if ((nfaStat = NFA_EeGetInfo(&actualNumEe, eeInfo)) != NFA_STATUS_OK) {
    LOG(ERROR) << StringPrintf("%s: fail get info; error=0x%X", fn, nfaStat);
    return;
  }
  if (actualNumEe != 0) {
    for (uint8_t xx = 0; xx < actualNumEe; xx++) {
      bool bIsOffHostEEPresent =
          (NFC_GetNCIVersion() < NCI_VERSION_2_0)
              ? (eeInfo[xx].num_interface != 0)
              : (eeInfo[xx].ee_interface[0] !=
                 NCI_NFCEE_INTERFACE_HCI_ACCESS) &&
                    (eeInfo[xx].ee_status == NFA_EE_STATUS_ACTIVE);
      if (bIsOffHostEEPresent) {
        LOG(DEBUG) << StringPrintf(
            "%s: Handle: 0x%04x Change Status Active to Inactive", fn,
            eeInfo[xx].ee_handle);
        SyncEventGuard guard(mEeSetModeEvent);
        if ((nfaStat = NFA_EeModeSet(eeInfo[xx].ee_handle,
                                     NFA_EE_MD_DEACTIVATE)) == NFA_STATUS_OK) {
          mEeSetModeEvent.wait();  // wait for NFA_EE_MODE_SET_EVT
        } else {
          LOG(ERROR) << fn << "Failed to set EE inactive";
        }
      }
    }
  } else {
    LOG(DEBUG) << fn << ": No active EEs found";
  }
}

void RoutingManager::notifyActivated(uint8_t technology) {
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  if (e == NULL) {
    LOG(ERROR) << "jni env is null";
    return;
  }

  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyHostEmuActivated,
                    (int)technology);
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << "fail notify";
  }
}

bool RoutingManager::getNameOfEe(tNFA_HANDLE ee_handle, std::string& eeName) {
  if (mOffHostRouteEse.size() == 0) {
    return false;
  }
  ee_handle &= ~NFA_HANDLE_GROUP_EE;

  for (uint8_t i = 0; i < mOffHostRouteEse.size(); i++) {
    if (ee_handle == mOffHostRouteEse[i]) {
      eeName = "eSE" + std::to_string(i + 1);
      return true;
    }
  }
  for (uint8_t i = 0; i < mOffHostRouteUicc.size(); i++) {
    if (ee_handle == mOffHostRouteUicc[i]) {
      eeName = "SIM" + std::to_string(i + 1);
      return true;
    }
  }

  LOG(WARNING) << "Incorrect EE Id";
  return false;
}

void RoutingManager::notifyEeAidSelected(tNFC_AID& nfcaid,
                                         tNFA_HANDLE ee_handle) {
  std::vector<uint8_t> aid(nfcaid.aid, nfcaid.aid + nfcaid.len_aid);
  if (aid.empty()) {
    return;
  }

  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  CHECK(e);

  ScopedLocalRef<jobject> aidJavaArray(e, e->NewByteArray(aid.size()));
  CHECK(aidJavaArray.get());
  e->SetByteArrayRegion((jbyteArray)aidJavaArray.get(), 0, aid.size(),
                        (jbyte*)&aid[0]);
  CHECK(!e->ExceptionCheck());

  std::string evtSrc;
  if (!getNameOfEe(ee_handle, evtSrc)) {
    return;
  }

  ScopedLocalRef<jobject> srcJavaString(e, e->NewStringUTF(evtSrc.c_str()));
  CHECK(srcJavaString.get());
  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyEeAidSelected,
                    aidJavaArray.get(), srcJavaString.get());
}

void RoutingManager::notifyEeProtocolSelected(uint8_t protocol,
                                              tNFA_HANDLE ee_handle) {
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  CHECK(e);

  std::string evtSrc;
  if (!getNameOfEe(ee_handle, evtSrc)) {
    return;
  }

  ScopedLocalRef<jobject> srcJavaString(e, e->NewStringUTF(evtSrc.c_str()));
  CHECK(srcJavaString.get());
  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyEeProtocolSelected,
                    protocol, srcJavaString.get());
}

void RoutingManager::notifyEeTechSelected(uint8_t tech, tNFA_HANDLE ee_handle) {
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  CHECK(e);

  std::string evtSrc;
  if (!getNameOfEe(ee_handle, evtSrc)) {
    return;
  }

  ScopedLocalRef<jobject> srcJavaString(e, e->NewStringUTF(evtSrc.c_str()));
  CHECK(srcJavaString.get());
  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyEeTechSelected, tech,
                    srcJavaString.get());
}

void RoutingManager::notifyDeactivated(uint8_t technology) {
  mRxDataBuffer.clear();
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  if (e == NULL) {
    LOG(ERROR) << "jni env is null";
    return;
  }

  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyEeListenActivated,
                    JNI_FALSE);
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << StringPrintf("Fail to notify Ee listen active status.");
  }

  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyHostEmuDeactivated,
                    (int)technology);
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << StringPrintf("fail notify");
  }
}

void RoutingManager::handleData(uint8_t technology, const uint8_t* data,
                                uint32_t dataLen, tNFA_STATUS status) {
  if (status == NFC_STATUS_CONTINUE) {
    if (dataLen > 0) {
      mRxDataBuffer.insert(mRxDataBuffer.end(), &data[0],
                           &data[dataLen]);  // append data; more to come
    }
    return;  // expect another NFA_CE_DATA_EVT to come
  } else if (status == NFA_STATUS_OK) {
    if (dataLen > 0) {
      mRxDataBuffer.insert(mRxDataBuffer.end(), &data[0],
                           &data[dataLen]);  // append data
    }
    // entire data packet has been received; no more NFA_CE_DATA_EVT
  } else if (status == NFA_STATUS_FAILED) {
    LOG(ERROR) << "RoutingManager::handleData: read data fail";
    goto TheEnd;
  }

  {
    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL) {
      LOG(ERROR) << "jni env is null";
      goto TheEnd;
    }

    ScopedLocalRef<jobject> dataJavaArray(
        e, e->NewByteArray(mRxDataBuffer.size()));
    if (dataJavaArray.get() == NULL) {
      LOG(ERROR) << "fail allocate array";
      goto TheEnd;
    }

    e->SetByteArrayRegion((jbyteArray)dataJavaArray.get(), 0,
                          mRxDataBuffer.size(), (jbyte*)(&mRxDataBuffer[0]));
    if (e->ExceptionCheck()) {
      e->ExceptionClear();
      LOG(ERROR) << "fail fill array";
      goto TheEnd;
    }

    e->CallVoidMethod(mNativeData->manager,
                      android::gCachedNfcManagerNotifyHostEmuData,
                      (int)technology, dataJavaArray.get());
    if (e->ExceptionCheck()) {
      e->ExceptionClear();
      LOG(ERROR) << "fail notify";
    }
  }
TheEnd:
  mRxDataBuffer.clear();
}

void RoutingManager::notifyEeUpdated() {
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  if (e == NULL) {
    LOG(ERROR) << "jni env is null";
    return;
  }

  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyEeUpdated);
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << "fail notify";
  }
}

void RoutingManager::stackCallback(uint8_t event,
                                   tNFA_CONN_EVT_DATA* eventData) {
  static const char fn[] = "RoutingManager::stackCallback";
  LOG(DEBUG) << StringPrintf("%s: event=0x%X", fn, event);
  RoutingManager& routingManager = RoutingManager::getInstance();

  switch (event) {
    case NFA_CE_REGISTERED_EVT: {
      tNFA_CE_REGISTERED& ce_registered = eventData->ce_registered;
      LOG(DEBUG) << StringPrintf(
          "%s: NFA_CE_REGISTERED_EVT; status=0x%X; h=0x%X", fn,
          ce_registered.status, ce_registered.handle);
    } break;

    case NFA_CE_DEREGISTERED_EVT: {
      tNFA_CE_DEREGISTERED& ce_deregistered = eventData->ce_deregistered;
      LOG(DEBUG) << StringPrintf("%s: NFA_CE_DEREGISTERED_EVT; h=0x%X", fn,
                                 ce_deregistered.handle);
    } break;

    case NFA_CE_ACTIVATED_EVT: {
      routingManager.notifyActivated(NFA_TECHNOLOGY_MASK_A);
    } break;

    case NFA_DEACTIVATED_EVT:
    case NFA_CE_DEACTIVATED_EVT: {
      LOG(DEBUG) << StringPrintf(
          "%s: NFA_DEACTIVATED_EVT, NFA_CE_DEACTIVATED_EVT", fn);
      routingManager.notifyDeactivated(NFA_TECHNOLOGY_MASK_A);
      SyncEventGuard g(gDeactivatedEvent);
      gActivated = false;  // guard this variable from multi-threaded access
      gDeactivatedEvent.notifyOne();
    } break;

    case NFA_CE_DATA_EVT: {
      tNFA_CE_DATA& ce_data = eventData->ce_data;
      LOG(DEBUG) << StringPrintf(
          "%s: NFA_CE_DATA_EVT; stat=0x%X; h=0x%X; data len=%u", fn,
          ce_data.status, ce_data.handle, ce_data.len);
      getInstance().handleData(NFA_TECHNOLOGY_MASK_A, ce_data.p_data,
                               ce_data.len, ce_data.status);
    } break;
  }
}

void RoutingManager::updateRoutingTable() {
  updateEeTechRouteSetting();
  updateDefaultProtocolRoute();
  updateDefaultRoute();
}

void RoutingManager::updateIsoDepProtocolRoute(int route) {
  static const char fn[] = "RoutingManager::updateIsoDepProtocolRoute";
  tNFA_PROTOCOL_MASK protoMask = NFA_PROTOCOL_MASK_ISO_DEP;
  tNFA_STATUS nfaStat;

  SyncEventGuard guard(mRoutingEvent);
  nfaStat = NFA_EeClearDefaultProtoRouting(mDefaultIsoDepRoute, protoMask);
  if (nfaStat == NFA_STATUS_OK)
    mRoutingEvent.wait();
  else
    LOG(ERROR) << fn << "Fail to clear IsoDep route";

  mDefaultIsoDepRoute = route;
  updateDefaultProtocolRoute();
}

/*******************************************************************************
**
** Function:        updateSystemCodeRoute
**
** Description:     Updates the route for System Code
**
** Returns:         None
**
*******************************************************************************/
void RoutingManager::updateSystemCodeRoute(int route) {
  static const char fn[] = "RoutingManager::updateSystemCodeRoute";
  LOG(DEBUG) << StringPrintf("%s; New default SC route: 0x%x", fn,
                             route);
  mEeInfoChanged = true;
  mDefaultSysCodeRoute = route;
  updateDefaultRoute();
}

void RoutingManager::updateDefaultProtocolRoute() {
  static const char fn[] = "RoutingManager::updateDefaultProtocolRoute";

  // Default Routing for ISO-DEP
  tNFA_PROTOCOL_MASK protoMask = NFA_PROTOCOL_MASK_ISO_DEP;
  tNFA_STATUS nfaStat;
  if (mDefaultIsoDepRoute != NFC_DH_ID &&
      isTypeATypeBTechSupportedInEe(mDefaultIsoDepRoute |
                                    NFA_HANDLE_GROUP_EE)) {
    nfaStat = NFA_EeClearDefaultProtoRouting(mDefaultIsoDepRoute, protoMask);
    nfaStat = NFA_EeSetDefaultProtoRouting(
        mDefaultIsoDepRoute, protoMask, mSecureNfcEnabled ? 0 : protoMask, 0,
        mSecureNfcEnabled ? 0 : protoMask, mSecureNfcEnabled ? 0 : protoMask,
        mSecureNfcEnabled ? 0 : protoMask);
  } else {
    nfaStat = NFA_EeClearDefaultProtoRouting(NFC_DH_ID, protoMask);
    nfaStat = NFA_EeSetDefaultProtoRouting(
        NFC_DH_ID, protoMask, 0, 0, mSecureNfcEnabled ? 0 : protoMask, 0, 0);
  }
  if (nfaStat == NFA_STATUS_OK)
    LOG(DEBUG) << fn << ": Succeed to register default ISO-DEP route";
  else
    LOG(ERROR) << fn << ": failed to register default ISO-DEP route";

  // Default routing for T3T protocol
  if (!mIsScbrSupported) {
    SyncEventGuard guard(mRoutingEvent);
    tNFA_PROTOCOL_MASK protoMask = NFA_PROTOCOL_MASK_T3T;
    if (mDefaultEe == NFC_DH_ID) {
      nfaStat =
          NFA_EeSetDefaultProtoRouting(NFC_DH_ID, protoMask, 0, 0, 0, 0, 0);
    } else {
      nfaStat = NFA_EeClearDefaultProtoRouting(mDefaultEe, protoMask);
      nfaStat = NFA_EeSetDefaultProtoRouting(
          mDefaultEe, protoMask, 0, 0, mSecureNfcEnabled ? 0 : protoMask,
          mSecureNfcEnabled ? 0 : protoMask, mSecureNfcEnabled ? 0 : protoMask);
    }
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default proto routing for T3T";
  }
}

void RoutingManager::updateDefaultRoute() {
  static const char fn[] = "RoutingManager::updateDefaultRoute";
  if (NFC_GetNCIVersion() != NCI_VERSION_2_0) return;

  // Register System Code for routing
  SyncEventGuard guard(mRoutingEvent);
  tNFA_STATUS nfaStat = NFA_EeAddSystemCodeRouting(
      mDefaultSysCode, mDefaultSysCodeRoute,
      mSecureNfcEnabled ? 0x01 : mDefaultSysCodePowerstate);
  if (nfaStat == NFA_STATUS_NOT_SUPPORTED) {
    mIsScbrSupported = false;
    LOG(ERROR) << fn << ": SCBR not supported";
  } else if (nfaStat == NFA_STATUS_OK) {
    mIsScbrSupported = true;
    mRoutingEvent.wait();
    LOG(DEBUG) << fn << ": Succeed to register system code";
  } else {
    LOG(ERROR) << fn << ": Fail to register system code";
    // still support SCBR routing for other NFCEEs
    mIsScbrSupported = true;
  }

  // Register zero lengthy Aid for default Aid Routing
  if (mDefaultEe != mDefaultIsoDepRoute) {
    if ((mDefaultEe != NFC_DH_ID) &&
        (!isTypeATypeBTechSupportedInEe(mDefaultEe | NFA_HANDLE_GROUP_EE))) {
      LOG(DEBUG)
          << fn << ": mDefaultEE Doesn't support either Tech A/B. Returning...";
      return;
    }
    uint8_t powerState = 0x01;
    if (!mSecureNfcEnabled)
      powerState = (mDefaultEe != 0x00) ? mOffHostAidRoutingPowerState : 0x11;
    nfaStat = NFA_EeAddAidRouting(mDefaultEe, 0, NULL, powerState,
                                  AID_ROUTE_QUAL_PREFIX);
    if (nfaStat == NFA_STATUS_OK)
      LOG(DEBUG) << fn << ": Succeed to register zero length AID";
    else
      LOG(ERROR) << fn << ": failed to register zero length AID";
  }
}

tNFA_TECHNOLOGY_MASK RoutingManager::updateTechnologyABFRoute(int route,
                                                              int felicaRoute) {
  static const char fn[] = "RoutingManager::updateTechnologyABFRoute";

  tNFA_STATUS nfaStat;

  SyncEventGuard guard(mRoutingEvent);
  nfaStat = NFA_EeClearDefaultTechRouting(
      mDefaultOffHostRoute,
      NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B | NFA_TECHNOLOGY_MASK_F);
  if (nfaStat == NFA_STATUS_OK)
    mRoutingEvent.wait();
  else
    LOG(ERROR) << fn << "Fail to clear Tech route";

  nfaStat =
      NFA_EeClearDefaultTechRouting(mDefaultFelicaRoute, NFA_TECHNOLOGY_MASK_F);
  if (nfaStat == NFA_STATUS_OK)
    mRoutingEvent.wait();
  else
    LOG(ERROR) << fn << "Fail to clear Default Felica route";

  mDefaultFelicaRoute = felicaRoute;
  mDefaultOffHostRoute = route;
  return updateEeTechRouteSetting();
}

tNFA_TECHNOLOGY_MASK RoutingManager::updateEeTechRouteSetting() {
  static const char fn[] = "RoutingManager::updateEeTechRouteSetting";
  tNFA_TECHNOLOGY_MASK allSeTechMask = 0x00;

  LOG(DEBUG) << fn << ": Number of EE is " << (int)mEeInfo.num_ee;

  tNFA_STATUS nfaStat;

  bool offHostRouteFound = false;
  bool felicaRouteFound = false;

  int defaultFelicaRoute = mDefaultFelicaRoute;
  int defaultOffHostRoute = mDefaultOffHostRoute;

  for (uint8_t i = 0; i < mEeInfo.num_ee; i++) {
    tNFA_HANDLE eeHandle = mEeInfo.ee_disc_info[i].ee_handle;
    tNFA_TECHNOLOGY_MASK seTechMask = 0;

    LOG(DEBUG) << StringPrintf(
        "%s   EE[%u] Handle: 0x%04x  techA: 0x%02x  techB: "
        "0x%02x  techF: 0x%02x  techBprime: 0x%02x",
        fn, i, eeHandle, mEeInfo.ee_disc_info[i].la_protocol,
        mEeInfo.ee_disc_info[i].lb_protocol,
        mEeInfo.ee_disc_info[i].lf_protocol,
        mEeInfo.ee_disc_info[i].lbp_protocol);

    if ((mDefaultOffHostRoute != 0) &&
        (eeHandle == (mDefaultOffHostRoute | NFA_HANDLE_GROUP_EE))) {
      offHostRouteFound = true;
      if (mEeInfo.ee_disc_info[i].la_protocol != 0)
        seTechMask |= NFA_TECHNOLOGY_MASK_A;
      if (mEeInfo.ee_disc_info[i].lb_protocol != 0)
        seTechMask |= NFA_TECHNOLOGY_MASK_B;
    }
    if ((mDefaultFelicaRoute != 0) &&
        (eeHandle == (mDefaultFelicaRoute | NFA_HANDLE_GROUP_EE))) {
      felicaRouteFound = true;
      if (mEeInfo.ee_disc_info[i].lf_protocol != 0)
        seTechMask |= NFA_TECHNOLOGY_MASK_F;
      else
        defaultFelicaRoute = NFC_DH_ID;
    }

    // If OFFHOST_LISTEN_TECH_MASK exists,
    // filter out the unspecified technologies
    seTechMask &= mOffHostListenTechMask;

    LOG(DEBUG) << StringPrintf("%s: seTechMask[%u]=0x%02x", fn, i, seTechMask);
    if (seTechMask != 0x00) {
      LOG(DEBUG) << StringPrintf("Configuring tech mask 0x%02x on EE 0x%04x",
                                 seTechMask, eeHandle);

      nfaStat = NFA_CeConfigureUiccListenTech(eeHandle, seTechMask);
      if (nfaStat != NFA_STATUS_OK)
        LOG(ERROR) << fn << "Failed to configure UICC listen technologies.";

      // clear previous before setting new power state
      nfaStat = NFA_EeClearDefaultTechRouting(eeHandle, seTechMask);
      if (nfaStat != NFA_STATUS_OK)
        LOG(ERROR) << fn << "Failed to clear EE technology routing.";

      nfaStat = NFA_EeSetDefaultTechRouting(
          eeHandle, seTechMask, mSecureNfcEnabled ? 0 : seTechMask, 0,
          mSecureNfcEnabled ? 0 : seTechMask,
          mSecureNfcEnabled ? 0 : seTechMask,
          mSecureNfcEnabled ? 0 : seTechMask);
      if (nfaStat != NFA_STATUS_OK)
        LOG(ERROR) << fn << "Failed to configure UICC technology routing.";

      allSeTechMask |= seTechMask;
    }
  }

  if (!offHostRouteFound) {
    defaultOffHostRoute = NFC_DH_ID;
  }
  if (!felicaRouteFound) {
    defaultFelicaRoute = NFC_DH_ID;
  }

  tNFA_TECHNOLOGY_MASK hostTechMask = 0;
  if (defaultOffHostRoute == NFC_DH_ID || defaultFelicaRoute == NFC_DH_ID) {
    if (defaultOffHostRoute == NFC_DH_ID) {
      LOG(DEBUG) << StringPrintf(
          "%s: Setting technology route to host with A,B type", fn);
      hostTechMask |= NFA_TECHNOLOGY_MASK_A;
      hostTechMask |= NFA_TECHNOLOGY_MASK_B;
    }
    if (defaultFelicaRoute == NFC_DH_ID) {
      LOG(DEBUG) << StringPrintf(
          "%s: Setting technology route to host with F type", fn);
      hostTechMask |= NFA_TECHNOLOGY_MASK_F;
    }
    hostTechMask &= mHostListenTechMask;
    nfaStat = NFA_EeSetDefaultTechRouting(NFC_DH_ID, hostTechMask, 0, 0,
                                          mSecureNfcEnabled ? 0 : hostTechMask,
                                          mSecureNfcEnabled ? 0 : hostTechMask,
                                          mSecureNfcEnabled ? 0 : hostTechMask);
    if (nfaStat != NFA_STATUS_OK)
      LOG(ERROR) << fn << "Failed to configure DH technology routing.";
    nfaStat = NFA_CeConfigureUiccListenTech(NFC_DH_ID, hostTechMask);
    if (nfaStat != NFA_STATUS_OK)
      LOG(ERROR) << fn << "Failed to configure DH UICC listen technologies.";
  }

  // Clear DH technology route on NFC-A
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_A) &&
      (allSeTechMask & NFA_TECHNOLOGY_MASK_A) != 0) {
    nfaStat = NFA_EeClearDefaultTechRouting(NFC_DH_ID, NFA_TECHNOLOGY_MASK_A);
    if (nfaStat != NFA_STATUS_OK)
      LOG(ERROR) << "Failed to clear DH technology routing on NFC-A.";
  }

  // Clear DH technology route on NFC-B
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_B) &&
      (allSeTechMask & NFA_TECHNOLOGY_MASK_B) != 0) {
    nfaStat = NFA_EeClearDefaultTechRouting(NFC_DH_ID, NFA_TECHNOLOGY_MASK_B);
    if (nfaStat != NFA_STATUS_OK)
      LOG(ERROR) << "Failed to clear DH technology routing on NFC-B.";
  }

  // Clear DH technology route on NFC-F
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_F) &&
      (allSeTechMask & NFA_TECHNOLOGY_MASK_F) != 0) {
    nfaStat = NFA_EeClearDefaultTechRouting(NFC_DH_ID, NFA_TECHNOLOGY_MASK_F);
    if (nfaStat != NFA_STATUS_OK)
      LOG(ERROR) << "Failed to clear DH technology routing on NFC-F.";
  }
  return allSeTechMask;
}

/*******************************************************************************
**
** Function:        nfaEeCallback
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void RoutingManager::nfaEeCallback(tNFA_EE_EVT event,
                                   tNFA_EE_CBACK_DATA* eventData) {
  static const char fn[] = "RoutingManager::nfaEeCallback";

  RoutingManager& routingManager = RoutingManager::getInstance();
  if (!eventData) {
    LOG(ERROR) << "eventData is null";
    return;
  }
  routingManager.mCbEventData = *eventData;
  switch (event) {
    case NFA_EE_REGISTER_EVT: {
      SyncEventGuard guard(routingManager.mEeRegisterEvent);
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_REGISTER_EVT; status=%u", fn,
                                 eventData->ee_register);
      routingManager.mEeRegisterEvent.notifyOne();
    } break;

    case NFA_EE_DEREGISTER_EVT: {
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_DEREGISTER_EVT; status=0x%X", fn,
                                 eventData->status);
      routingManager.mReceivedEeInfo = false;
      routingManager.mDeinitializing = false;
    } break;

    case NFA_EE_MODE_SET_EVT: {
      SyncEventGuard guard(routingManager.mEeSetModeEvent);
      LOG(DEBUG) << StringPrintf(
          "%s: NFA_EE_MODE_SET_EVT; status: 0x%04X  handle: 0x%04X  ", fn,
          eventData->mode_set.status, eventData->mode_set.ee_handle);
      routingManager.mEeSetModeEvent.notifyOne();
    } break;

    case NFA_EE_SET_TECH_CFG_EVT: {
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_SET_TECH_CFG_EVT; status=0x%X", fn,
                                 eventData->status);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;

    case NFA_EE_CLEAR_TECH_CFG_EVT: {
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_CLEAR_TECH_CFG_EVT; status=0x%X",
                                 fn, eventData->status);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;

    case NFA_EE_SET_PROTO_CFG_EVT: {
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_SET_PROTO_CFG_EVT; status=0x%X",
                                 fn, eventData->status);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;

    case NFA_EE_CLEAR_PROTO_CFG_EVT: {
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_CLEAR_PROTO_CFG_EVT; status=0x%X",
                                 fn, eventData->status);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;

    case NFA_EE_ACTION_EVT: {
      tNFA_EE_ACTION& action = eventData->action;
      if (action.trigger == NFC_EE_TRIG_SELECT) {
        tNFC_AID& aid = action.param.aid;
        LOG(DEBUG) << StringPrintf(
            "%s: NFA_EE_ACTION_EVT; h=0x%X; trigger=select (0x%X)", fn,
            action.ee_handle, action.trigger);
        routingManager.notifyEeAidSelected(aid, action.ee_handle);
      } else if (action.trigger == NFC_EE_TRIG_APP_INIT) {
        tNFC_APP_INIT& app_init = action.param.app_init;
        LOG(DEBUG) << StringPrintf(
            "%s: NFA_EE_ACTION_EVT; h=0x%X; trigger=app-init "
            "(0x%X); aid len=%u; data len=%u",
            fn, action.ee_handle, action.trigger, app_init.len_aid,
            app_init.len_data);
      } else if (action.trigger == NFC_EE_TRIG_RF_PROTOCOL) {
        LOG(DEBUG) << StringPrintf(
            "%s: NFA_EE_ACTION_EVT; h=0x%X; trigger=rf protocol (0x%X)", fn,
            action.ee_handle, action.trigger);
        routingManager.notifyEeProtocolSelected(action.param.protocol,
                                                  action.ee_handle);
      } else if (action.trigger == NFC_EE_TRIG_RF_TECHNOLOGY) {
        LOG(DEBUG) << StringPrintf(
            "%s: NFA_EE_ACTION_EVT; h=0x%X; trigger=rf tech (0x%X)", fn,
            action.ee_handle, action.trigger);
        routingManager.notifyEeTechSelected(action.param.technology,
                                              action.ee_handle);
      } else
        LOG(DEBUG) << StringPrintf(
            "%s: NFA_EE_ACTION_EVT; h=0x%X; unknown trigger (0x%X)", fn,
            action.ee_handle, action.trigger);
    } break;

    case NFA_EE_DISCOVER_REQ_EVT: {
      LOG(DEBUG) << StringPrintf(
          "%s: NFA_EE_DISCOVER_REQ_EVT; status=0x%X; num ee=%u", __func__,
          eventData->discover_req.status, eventData->discover_req.num_ee);
      SyncEventGuard guard(routingManager.mEeInfoEvent);
      memcpy(&routingManager.mEeInfo, &eventData->discover_req,
             sizeof(routingManager.mEeInfo));
      if (routingManager.mReceivedEeInfo && !routingManager.mDeinitializing) {
        routingManager.mEeInfoChanged = true;
        routingManager.notifyEeUpdated();
      }
      routingManager.mReceivedEeInfo = true;
      routingManager.mEeInfoEvent.notifyOne();
    } break;

    case NFA_EE_NO_CB_ERR_EVT:
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_NO_CB_ERR_EVT  status=%u", fn,
                                 eventData->status);
      break;

    case NFA_EE_ADD_AID_EVT: {
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_ADD_AID_EVT  status=%u", fn,
                                 eventData->status);
      SyncEventGuard guard(routingManager.mAidAddRemoveEvent);
      routingManager.mAidRoutingConfigured =
          (eventData->status == NFA_STATUS_OK);
      routingManager.mAidAddRemoveEvent.notifyOne();
    } break;

    case NFA_EE_ADD_SYSCODE_EVT: {
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_ADD_SYSCODE_EVT  status=%u", fn,
                                 eventData->status);
    } break;

    case NFA_EE_REMOVE_SYSCODE_EVT: {
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_REMOVE_SYSCODE_EVT  status=%u", fn,
                                 eventData->status);
    } break;

    case NFA_EE_REMOVE_AID_EVT: {
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_REMOVE_AID_EVT  status=%u", fn,
                                 eventData->status);
      SyncEventGuard guard(routingManager.mAidAddRemoveEvent);
      routingManager.mAidRoutingConfigured =
          (eventData->status == NFA_STATUS_OK);
      routingManager.mAidAddRemoveEvent.notifyOne();
    } break;

    case NFA_EE_NEW_EE_EVT: {
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_NEW_EE_EVT  h=0x%X; status=%u", fn,
                                 eventData->new_ee.ee_handle,
                                 eventData->new_ee.ee_status);
    } break;

    case NFA_EE_UPDATED_EVT: {
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_UPDATED_EVT", fn);
      SyncEventGuard guard(routingManager.mEeUpdateEvent);
      routingManager.mEeUpdateEvent.notifyOne();
    } break;

    case NFA_EE_PWR_AND_LINK_CTRL_EVT: {
      LOG(DEBUG) << StringPrintf("%s: NFA_EE_PWR_AND_LINK_CTRL_EVT", fn);
      SyncEventGuard guard(routingManager.mEePwrAndLinkCtrlEvent);
      routingManager.mEePwrAndLinkCtrlEvent.notifyOne();
    } break;

    default:
      LOG(DEBUG) << StringPrintf("%s: unknown event=%u ????", fn, event);
      break;
  }
}

int RoutingManager::registerT3tIdentifier(uint8_t* t3tId, uint8_t t3tIdLen) {
  static const char fn[] = "RoutingManager::registerT3tIdentifier";

  LOG(DEBUG) << fn << ": Start to register NFC-F system on DH";

  if (t3tIdLen != (2 + NCI_RF_F_UID_LEN + NCI_T3T_PMM_LEN)) {
    LOG(ERROR) << fn << ": Invalid length of T3T Identifier";
    return NFA_HANDLE_INVALID;
  }

  mNfcFOnDhHandle = NFA_HANDLE_INVALID;

  uint16_t systemCode;
  uint8_t nfcid2[NCI_RF_F_UID_LEN];
  uint8_t t3tPmm[NCI_T3T_PMM_LEN];

  systemCode = (((int)t3tId[0] << 8) | ((int)t3tId[1] << 0));
  memcpy(nfcid2, t3tId + 2, NCI_RF_F_UID_LEN);
  memcpy(t3tPmm, t3tId + 10, NCI_T3T_PMM_LEN);
  {
    SyncEventGuard guard(mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_CeRegisterFelicaSystemCodeOnDH(
        systemCode, nfcid2, t3tPmm, nfcFCeCallback);
    if (nfaStat == NFA_STATUS_OK) {
      mRoutingEvent.wait();
    } else {
      LOG(ERROR) << fn << ": Fail to register NFC-F system on DH";
      return NFA_HANDLE_INVALID;
    }
  }
  LOG(DEBUG) << fn << ": Succeed to register NFC-F system on DH";

  // Register System Code for routing
  if (mIsScbrSupported) {
    SyncEventGuard guard(mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_EeAddSystemCodeRouting(systemCode, NCI_DH_ID,
                                                     SYS_CODE_PWR_STATE_HOST);
    if (nfaStat == NFA_STATUS_OK) {
      mRoutingEvent.wait();
    }
    if ((nfaStat != NFA_STATUS_OK) || (mCbEventData.status != NFA_STATUS_OK)) {
      LOG(ERROR) << StringPrintf("%s: Fail to register system code on DH", fn);
      return NFA_HANDLE_INVALID;
    }
    LOG(DEBUG) << StringPrintf("%s: Succeed to register system code on DH", fn);
    // add handle and system code pair to the map
    mMapScbrHandle.emplace(mNfcFOnDhHandle, systemCode);
  } else {
    LOG(ERROR) << StringPrintf("%s: SCBR Not supported", fn);
  }

  return mNfcFOnDhHandle;
}

void RoutingManager::deregisterT3tIdentifier(int handle) {
  static const char fn[] = "RoutingManager::deregisterT3tIdentifier";

  LOG(DEBUG) << StringPrintf("%s: Start to deregister NFC-F system on DH", fn);
  {
    SyncEventGuard guard(mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_CeDeregisterFelicaSystemCodeOnDH(handle);
    if (nfaStat == NFA_STATUS_OK) {
      mRoutingEvent.wait();
      LOG(DEBUG) << StringPrintf(
          "%s: Succeeded in deregistering NFC-F system on DH", fn);
    } else {
      LOG(ERROR) << StringPrintf("%s: Fail to deregister NFC-F system on DH",
                                 fn);
    }
  }
  if (mIsScbrSupported) {
    map<int, uint16_t>::iterator it = mMapScbrHandle.find(handle);
    // find system code for given handle
    if (it != mMapScbrHandle.end()) {
      uint16_t systemCode = it->second;
      mMapScbrHandle.erase(handle);
      if (systemCode != 0) {
        SyncEventGuard guard(mRoutingEvent);
        tNFA_STATUS nfaStat = NFA_EeRemoveSystemCodeRouting(systemCode);
        if (nfaStat == NFA_STATUS_OK) {
          mRoutingEvent.wait();
          LOG(DEBUG) << StringPrintf(
              "%s: Succeeded in deregistering system Code on DH", fn);
        } else {
          LOG(ERROR) << StringPrintf("%s: Fail to deregister system Code on DH",
                                     fn);
        }
      }
    }
  }
}

void RoutingManager::nfcFCeCallback(uint8_t event,
                                    tNFA_CONN_EVT_DATA* eventData) {
  static const char fn[] = "RoutingManager::nfcFCeCallback";
  RoutingManager& routingManager = RoutingManager::getInstance();

  LOG(DEBUG) << StringPrintf("%s: 0x%x", __func__, event);

  switch (event) {
    case NFA_CE_REGISTERED_EVT: {
      LOG(DEBUG) << StringPrintf("%s: registered event notified", fn);
      routingManager.mNfcFOnDhHandle = eventData->ce_registered.handle;
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;
    case NFA_CE_DEREGISTERED_EVT: {
      LOG(DEBUG) << StringPrintf("%s: deregistered event notified", fn);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;
    case NFA_CE_ACTIVATED_EVT: {
      LOG(DEBUG) << StringPrintf("%s: activated event notified", fn);
      routingManager.notifyActivated(NFA_TECHNOLOGY_MASK_F);
    } break;
    case NFA_CE_DEACTIVATED_EVT: {
      LOG(DEBUG) << StringPrintf("%s: deactivated event notified", fn);
      routingManager.notifyDeactivated(NFA_TECHNOLOGY_MASK_F);
    } break;
    case NFA_CE_DATA_EVT: {
      LOG(DEBUG) << StringPrintf("%s: data event notified", fn);
      tNFA_CE_DATA& ce_data = eventData->ce_data;
      routingManager.handleData(NFA_TECHNOLOGY_MASK_F, ce_data.p_data,
                                ce_data.len, ce_data.status);
    } break;
    default: {
      LOG(DEBUG) << StringPrintf("%s: unknown event=%u ????", fn, event);
    } break;
  }
}

/*******************************************************************************
**
** Function:        setNfcSecure
**
** Description:     get the NFC secure status
**
** Returns:         true
**
*******************************************************************************/
bool RoutingManager::setNfcSecure(bool enable) {
  mSecureNfcEnabled = enable;
  LOG(INFO) << "setNfcSecure NfcService " << enable;
  NFA_SetNfcSecure(enable);
  return true;
}

/*******************************************************************************
**
** Function:        eeSetPwrAndLinkCtrl
**
** Description:     Programs the NCI command NFCEE_POWER_AND_LINK_CTRL_CMD
**
** Returns:         None
**
*******************************************************************************/
void RoutingManager::eeSetPwrAndLinkCtrl(uint8_t config) {
  static const char fn[] = "RoutingManager::eeSetPwrAndLinkCtrl";
  tNFA_STATUS status = NFA_STATUS_OK;

  if (mOffHostRouteEse.size() > 0) {
    LOG(DEBUG) << StringPrintf("%s - nfceeId: 0x%02X, config: 0x%02X", fn,
                               mOffHostRouteEse[0], config);

    SyncEventGuard guard(mEePwrAndLinkCtrlEvent);
    status =
        NFA_EePowerAndLinkCtrl(
            ((uint8_t)mOffHostRouteEse[0] | NFA_HANDLE_GROUP_EE), config);
    if (status != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf("%s: fail NFA_EePowerAndLinkCtrl; error=0x%X",
                                 __FUNCTION__, status);
      return;
    } else {
      mEePwrAndLinkCtrlEvent.wait();
    }
  } else {
    LOG(ERROR) << StringPrintf("%s: No ESE specified", __FUNCTION__);
  }
}

void RoutingManager::clearRoutingEntry(int clearFlags) {
  static const char fn[] = "RoutingManager::clearRoutingEntry";

  LOG(DEBUG) << StringPrintf("%s: Enter . Clear flags = %d", fn, clearFlags);
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;

  if (clearFlags & CLEAR_AID_ENTRIES) {
    LOG(DEBUG) << StringPrintf("clear all of aid based routing");
    uint8_t clearAID[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t aidLen = 0x08;
    RoutingManager::getInstance().removeAidRouting(clearAID, aidLen);
  }

  if (clearFlags & CLEAR_PROTOCOL_ENTRIES) {
    for (uint8_t i = 0; i < mEeInfo.num_ee; i++) {
      tNFA_HANDLE eeHandle = mEeInfo.ee_disc_info[i].ee_handle;
      {
        SyncEventGuard guard(mRoutingEvent);
        nfaStat =
            NFA_EeClearDefaultProtoRouting(eeHandle, NFA_PROTOCOL_MASK_ISO_DEP);
        if (nfaStat == NFA_STATUS_OK) {
          mRoutingEvent.wait();
        }
      }
    }

    {
      SyncEventGuard guard(mRoutingEvent);
      nfaStat =
          NFA_EeClearDefaultProtoRouting(NFC_DH_ID, NFA_PROTOCOL_MASK_ISO_DEP);
      if (nfaStat == NFA_STATUS_OK) {
        mRoutingEvent.wait();
      }
    }
  }

  if (clearFlags & CLEAR_TECHNOLOGY_ENTRIES) {
    for (uint8_t i = 0; i < mEeInfo.num_ee; i++) {
      tNFA_HANDLE eeHandle = mEeInfo.ee_disc_info[i].ee_handle;
      {
        SyncEventGuard guard(mRoutingEvent);
        nfaStat = NFA_EeClearDefaultTechRouting(
            eeHandle, (NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B |
                       NFA_TECHNOLOGY_MASK_F));
        if (nfaStat == NFA_STATUS_OK) {
          mRoutingEvent.wait();
        }
      }
    }

    {
      SyncEventGuard guard(mRoutingEvent);
      nfaStat = NFA_EeClearDefaultTechRouting(
          NFC_DH_ID, (NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B |
                      NFA_TECHNOLOGY_MASK_F));
      if (nfaStat == NFA_STATUS_OK) {
        mRoutingEvent.wait();
      }
    }
  }
}

/*******************************************************************************
**
** Function:        setEeTechRouteUpdateRequired
**
** Description:     Set flag EeInfoChanged so that tech route will be updated
**                  when applying route table.
**
** Returns:         None
**
*******************************************************************************/
void RoutingManager::setEeTechRouteUpdateRequired() {
  static const char fn[] = "RoutingManager::setEeTechRouteUpdateRequired";

  LOG(DEBUG) << StringPrintf("%s", fn);

  // Setting flag for Ee info changed so that
  // routing table can be updated
  mEeInfoChanged = true;
}

void RoutingManager::deinitialize() {
  onNfccShutdown();
  NFA_EeDeregister(nfaEeCallback);
}

int RoutingManager::registerJniFunctions(JNIEnv* e) {
  static const char fn[] = "RoutingManager::registerJniFunctions";
  LOG(DEBUG) << StringPrintf("%s", fn);
  return jniRegisterNativeMethods(
      e, "com/android/nfc/cardemulation/RoutingOptionManager", sMethods,
      NELEM(sMethods));
}

int RoutingManager::com_android_nfc_cardemulation_doGetDefaultRouteDestination(
    JNIEnv*) {
  return getInstance().mDefaultEe;
}

int RoutingManager::
    com_android_nfc_cardemulation_doGetDefaultOffHostRouteDestination(JNIEnv*) {
  return getInstance().mDefaultOffHostRoute;
}

int RoutingManager::
    com_android_nfc_cardemulation_doGetDefaultFelicaRouteDestination(JNIEnv*) {
  return getInstance().mDefaultFelicaRoute;
}

jbyteArray
RoutingManager::com_android_nfc_cardemulation_doGetOffHostUiccDestination(
    JNIEnv* e) {
  std::vector<uint8_t> uicc = getInstance().mOffHostRouteUicc;
  if (uicc.size() == 0) {
    return NULL;
  }
  CHECK(e);
  jbyteArray uiccJavaArray = e->NewByteArray(uicc.size());
  CHECK(uiccJavaArray);
  e->SetByteArrayRegion(uiccJavaArray, 0, uicc.size(), (jbyte*)&uicc[0]);
  return uiccJavaArray;
}

jbyteArray
RoutingManager::com_android_nfc_cardemulation_doGetOffHostEseDestination(
    JNIEnv* e) {
  std::vector<uint8_t> ese = getInstance().mOffHostRouteEse;
  if (ese.size() == 0) {
    return NULL;
  }
  CHECK(e);
  jbyteArray eseJavaArray = e->NewByteArray(ese.size());
  CHECK(eseJavaArray);
  e->SetByteArrayRegion(eseJavaArray, 0, ese.size(), (jbyte*)&ese[0]);
  return eseJavaArray;
}

int RoutingManager::com_android_nfc_cardemulation_doGetAidMatchingMode(
    JNIEnv*) {
  return getInstance().mAidMatchingMode;
}

int RoutingManager::
    com_android_nfc_cardemulation_doGetDefaultIsoDepRouteDestination(JNIEnv*) {
  return getInstance().mDefaultIsoDepRoute;
}

/*******************************************************************************
**
** Function:        com_android_nfc_cardemulation_doGetDefaultScRouteDestination
**
** Description:     Retrieves the default NFCEE route
**
** Returns:         default NFCEE route
**
*******************************************************************************/
int RoutingManager::com_android_nfc_cardemulation_doGetDefaultScRouteDestination(
    JNIEnv*) {
  return getInstance().mDefaultSysCodeRoute;
}
