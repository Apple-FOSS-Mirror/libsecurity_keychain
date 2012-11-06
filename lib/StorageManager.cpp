/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All Rights Reserved.
 * 
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */


/*
	File:		StorageManager.cpp

	Contains:	Working with multiple keychains

*/

#include "StorageManager.h"
#include "KCEventNotifier.h"

#include <Security/cssmapple.h>
#include <sys/types.h>
#include <sys/param.h>
#include <syslog.h>
#include <pwd.h>
#include <CoreServices/../Frameworks/CarbonCore.framework/Headers/MacErrors.h>
#include <algorithm>
#include <string>
#include <stdio.h>
//#include <Security/AuthorizationTags.h>
//#include <Security/AuthSession.h>
#include <security_utilities/debugging.h>
#include <security_keychain/SecCFTypes.h>
//#include <Security/SecurityAgentClient.h>
#include <securityd_client/ssclient.h>
#include <Security/AuthorizationTags.h>
#include <Security/AuthorizationTagsPriv.h>

//%%% add this to AuthorizationTagsPriv.h later
#ifndef AGENT_HINT_LOGIN_KC_SUPPRESS_RESET_PANEL
#define AGENT_HINT_LOGIN_KC_SUPPRESS_RESET_PANEL "loginKCCreate:suppressResetPanel"
#endif

#include "KCCursor.h"
#include "Globals.h"


using namespace CssmClient;
using namespace KeychainCore;

// normal debug calls, which get stubbed out for deployment builds
#define x_debug(str) secdebug("KClogin",(str))
#define x_debug1(fmt,arg1) secdebug("KClogin",(fmt),(arg1))
#define x_debug2(fmt,arg1,arg2) secdebug("KClogin",(fmt),(arg1),(arg2))
#define kLoginKeychainPathPrefix "~/Library/Keychains/"
#define kUserLoginKeychainPath "~/Library/Keychains/login.keychain"


//-----------------------------------------------------------------------------------

StorageManager::StorageManager() :
	mSavedList(kSecPreferencesDomainUser),
	mCommonList(kSecPreferencesDomainCommon),
	mDomain(kSecPreferencesDomainUser)
{
	// get session attributes
	SessionAttributeBits sessionAttrs;
	if (gServerMode) {
		secdebug("servermode", "StorageManager initialized in server mode");
		sessionAttrs = sessionIsRoot;
	} else {
		// this forces a connection to securityd
		MacOSError::check(SessionGetInfo(callerSecuritySession,
			NULL, &sessionAttrs));
	}
	
	// If this is the root session, switch to system preferences.
	// (In SecurityServer debug mode, you'll get a (fake) root session
	// that has graphics access. Ignore that to help testing.)
	if ((sessionAttrs & sessionIsRoot)
			IFDEBUG( && !(sessionAttrs & sessionHasGraphicAccess))) {
		secdebug("storagemgr", "switching to system preferences");
		mDomain = kSecPreferencesDomainSystem;
		mSavedList.set(kSecPreferencesDomainSystem);
	}
}

Keychain
StorageManager::keychain(const DLDbIdentifier &dLDbIdentifier)
{
	StLock<Mutex> stAPILock(globals().apiLock);

	if (!dLDbIdentifier)
		return Keychain();
	
	if (gServerMode) {
		secdebug("servermode", "keychain reference in server mode");
		return Keychain();
	}

    KeychainMap::iterator it = mKeychains.find(dLDbIdentifier);
    if (it != mKeychains.end())
		return it->second;

	// The keychain is not in our cache.  Create it.
	Module module(dLDbIdentifier.ssuid().guid());
	DL dl;
	if (dLDbIdentifier.ssuid().subserviceType() & CSSM_SERVICE_CSP)
		dl = SSCSPDL(module);
	else
		dl = DL(module);

	dl->subserviceId(dLDbIdentifier.ssuid().subserviceId());
	dl->version(dLDbIdentifier.ssuid().version());
	Db db(dl, dLDbIdentifier.dbName());

	Keychain keychain(db);
	// Add the keychain to the cache.
	mKeychains.insert(KeychainMap::value_type(dLDbIdentifier, &*keychain));
	keychain->inCache(true);

	return keychain;
}

void 
StorageManager::removeKeychain(const DLDbIdentifier &dLDbIdentifier,
	KeychainImpl *keychainImpl)
{
	// Lock the recursive mutex
	StLock<Mutex> stAPILock(globals().apiLock);

	// If this keychain isn't in the map anymore we're done
	if (!keychainImpl->inCache())
		return;

	KeychainMap::iterator it = mKeychains.find(dLDbIdentifier);
	assert(it != mKeychains.end() && it->second == keychainImpl);
	if (it != mKeychains.end() && it->second == keychainImpl)
		mKeychains.erase(it);

	keychainImpl->inCache(false);
}

void 
StorageManager::didRemoveKeychain(const DLDbIdentifier &dLDbIdentifier)
{
	// Lock the recursive mutex
	StLock<Mutex> stAPILock(globals().apiLock);
	KeychainMap::iterator it = mKeychains.find(dLDbIdentifier);
	if (it != mKeychains.end())
	{
		KeychainImpl *keychainImpl = it->second;
		assert(keychainImpl->inCache());
		mKeychains.erase(it);
		keychainImpl->inCache(false);
	}
}

// Create keychain if it doesn't exist, add it to the search list if add is
// true, it exists and it is not already on it.
Keychain
StorageManager::makeKeychain(const DLDbIdentifier &dLDbIdentifier, bool add)
{
	bool post = false;

	Keychain theKeychain = keychain(dLDbIdentifier);
	if (add)
	{
		mSavedList.revert(false);
		DLDbList searchList = mSavedList.searchList();
		if (find(searchList.begin(), searchList.end(), dLDbIdentifier) != searchList.end())
			return theKeychain;  // theKeychain is already in the searchList.

		mCommonList.revert(false);
		searchList = mCommonList.searchList();
		if (find(searchList.begin(), searchList.end(), dLDbIdentifier) != searchList.end())
			return theKeychain;  // theKeychain is already in the commonList don't add it to the searchList.
	
		// If theKeychain doesn't exist don't bother adding it to the search list yet.
		if (!theKeychain->exists())
			return theKeychain;
	
		// theKeychain exists and is not in our search list, so add it to the 
		// search list.
		mSavedList.revert(true);
		mSavedList.add(dLDbIdentifier);
		mSavedList.save();
		post = true;
	}

	if (post)
	{
		// Make sure we are not holding mStorageManagerLock anymore when we
		// post this event.
		KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);
	}

	return theKeychain;
}

// Be notified a Keychain just got created.
void
StorageManager::created(const Keychain &keychain)
{
    DLDbIdentifier dLDbIdentifier = keychain->dlDbIdentifier();
	bool defaultChanged = false;

 	{
		mSavedList.revert(true);
		// If we don't have a default Keychain yet.  Make the newly created
		// keychain the default.
		if (!mSavedList.defaultDLDbIdentifier())
		{
			mSavedList.defaultDLDbIdentifier(dLDbIdentifier);
			defaultChanged = true;
		}

		// Add the keychain to the search list prefs.
		mSavedList.add(dLDbIdentifier);
		mSavedList.save();
	}

	// Make sure we are not holding mLock when we post these events.
	KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);

	if (defaultChanged)
	{
		KCEventNotifier::PostKeychainEvent(kSecDefaultChangedEvent, dLDbIdentifier);
	}
}

KCCursor
StorageManager::createCursor(SecItemClass itemClass,
	const SecKeychainAttributeList *attrList)
{
	KeychainList searchList;
	getSearchList(searchList);
	return KCCursor(searchList, itemClass, attrList);
}

KCCursor
StorageManager::createCursor(const SecKeychainAttributeList *attrList)
{
	KeychainList searchList;
	getSearchList(searchList);
	return KCCursor(searchList, attrList);
}

void
StorageManager::lockAll()
{
    SecurityServer::ClientSession ss(Allocator::standard(), Allocator::standard());
    ss.lockAll (false);
}

Keychain
StorageManager::defaultKeychain()
{
	Keychain theKeychain;
	{
		mSavedList.revert(false);
		DLDbIdentifier defaultDLDbIdentifier(mSavedList.defaultDLDbIdentifier());
		if (defaultDLDbIdentifier)
		{
			theKeychain = keychain(defaultDLDbIdentifier);
		}
	}

	if (theKeychain /* && theKeychain->exists() */)
		return theKeychain;

	MacOSError::throwMe(errSecNoDefaultKeychain);
}

void
StorageManager::defaultKeychain(const Keychain &keychain)
{
	DLDbIdentifier oldDefaultId;
	DLDbIdentifier newDefaultId(keychain->dlDbIdentifier());
	{
		oldDefaultId = mSavedList.defaultDLDbIdentifier();
		mSavedList.revert(true);
		mSavedList.defaultDLDbIdentifier(newDefaultId);
		mSavedList.save();
	}

	if (!(oldDefaultId == newDefaultId))
	{
		// Make sure we are not holding mLock when we post this event.
		KCEventNotifier::PostKeychainEvent(kSecDefaultChangedEvent, newDefaultId);
	}
}

Keychain
StorageManager::defaultKeychain(SecPreferencesDomain domain)
{
	if (domain == kSecPreferencesDomainDynamic)
		MacOSError::throwMe(errSecInvalidPrefsDomain);

	if (domain == mDomain)
		return defaultKeychain();
	else
	{
		DLDbIdentifier defaultDLDbIdentifier(DLDbListCFPref(domain).defaultDLDbIdentifier());
		if (defaultDLDbIdentifier)
			return keychain(defaultDLDbIdentifier);

		MacOSError::throwMe(errSecNoDefaultKeychain);
	}
}

void
StorageManager::defaultKeychain(SecPreferencesDomain domain, const Keychain &keychain)
{
	if (domain == kSecPreferencesDomainDynamic)
		MacOSError::throwMe(errSecInvalidPrefsDomain);

	if (domain == mDomain)
		defaultKeychain(keychain);
	else
		DLDbListCFPref(domain).defaultDLDbIdentifier(keychain->dlDbIdentifier());
}

Keychain
StorageManager::loginKeychain()
{
	Keychain theKeychain;
	{
		mSavedList.revert(false);
		DLDbIdentifier loginDLDbIdentifier(mSavedList.loginDLDbIdentifier());
		if (loginDLDbIdentifier)
		{
			theKeychain = keychain(loginDLDbIdentifier);
		}
	}

	if (theKeychain && theKeychain->exists())
		return theKeychain;

	MacOSError::throwMe(errSecNoSuchKeychain);
}

void
StorageManager::loginKeychain(Keychain keychain)
{
	mSavedList.revert(true);
	mSavedList.loginDLDbIdentifier(keychain->dlDbIdentifier());
	mSavedList.save();
}

size_t
StorageManager::size()
{
    mSavedList.revert(false);
	mCommonList.revert(false);
	return mSavedList.searchList().size() + mCommonList.searchList().size();
}

Keychain
StorageManager::at(unsigned int ix)
{
	mSavedList.revert(false);
	DLDbList dLDbList = mSavedList.searchList();
	if (ix < dLDbList.size())
	{
		return keychain(dLDbList[ix]);
	}
	else
	{
		ix -= dLDbList.size();
		mCommonList.revert(false);
		DLDbList commonList = mCommonList.searchList();
		if (ix >= commonList.size())
			MacOSError::throwMe(errSecInvalidKeychain);

		return keychain(commonList[ix]);
	}
}

Keychain
StorageManager::operator[](unsigned int ix)
{
    return at(ix);
}	

void StorageManager::rename(Keychain keychain, const char* newName)
{
    bool changedDefault = false;
	DLDbIdentifier newDLDbIdentifier;
	{
		mSavedList.revert(true);
		DLDbIdentifier defaultId = mSavedList.defaultDLDbIdentifier();

        // Find the keychain object for the given ref
        DLDbIdentifier dLDbIdentifier = keychain->dlDbIdentifier();

		// Actually rename the database on disk.
        keychain->database()->rename(newName);

        if (dLDbIdentifier == defaultId)
            changedDefault=true;

		newDLDbIdentifier = keychain->dlDbIdentifier();
        // Rename the keychain in the search list.
        mSavedList.rename(dLDbIdentifier, newDLDbIdentifier);

		// If this was the default keychain change it accordingly
		if (changedDefault)
			mSavedList.defaultDLDbIdentifier(newDLDbIdentifier);

		mSavedList.save();

		// Now update the Keychain cache
		StLock<Mutex> stAPILock(globals().apiLock);
		if (keychain->inCache())
		{
			KeychainMap::iterator it = mKeychains.find(dLDbIdentifier);
			assert(it != mKeychains.end() && it->second == keychain.get());
			if (it != mKeychains.end() && it->second == keychain.get())
			{
				// Remove the keychain from the cache under it's old
				// dLDbIdentifier
				mKeychains.erase(it);
			}
		}

		// If we renamed this keychain on top of an existing one we should
		// drop the old one from the cache.
		KeychainMap::iterator it = mKeychains.find(newDLDbIdentifier);
		if (it != mKeychains.end())
		{
			Keychain oldKeychain(it->second);
			oldKeychain->inCache(false);
			// @@@ Ideally we should invalidate or fault this keychain object.
		}

		if (keychain->inCache())
		{
			// If the keychain wasn't in the cache to being with let's not put
			// it there now.  There was probably a good reason it wasn't in it.
			// If the keychain was in the cache, update it to use
			// newDLDbIdentifier.
			mKeychains.insert(KeychainMap::value_type(newDLDbIdentifier,
				keychain));
		}
	}

	// Make sure we are not holding mLock when we post these events.
	KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);

	if (changedDefault)
		KCEventNotifier::PostKeychainEvent(kSecDefaultChangedEvent,
			newDLDbIdentifier);
}

void StorageManager::renameUnique(Keychain keychain, CFStringRef newName)
{
    bool doneCreating = false;
    int index = 1;
    do
    {
        char newNameCString[MAXPATHLEN];
        if ( CFStringGetCString(newName, newNameCString, MAXPATHLEN, kCFStringEncodingUTF8) )	// make sure it fits in MAXPATHLEN, etc.
        {
            // Construct the new name...
            //
            CFMutableStringRef newNameCFStr = NULL;
            newNameCFStr = CFStringCreateMutable(NULL, MAXPATHLEN); 
            if ( newNameCFStr )
            {
                CFStringAppendFormat(newNameCFStr, NULL, CFSTR("%s%d"), &newNameCString, index);
                CFStringAppend(newNameCFStr, CFSTR(kKeychainSuffix));	// add .keychain
                char toUseBuff2[MAXPATHLEN];
                if ( CFStringGetCString(newNameCFStr, toUseBuff2, MAXPATHLEN, kCFStringEncodingUTF8) )	// make sure it fits in MAXPATHLEN, etc.
                {
                    struct stat filebuf;
                    if ( lstat(toUseBuff2, &filebuf) )
                    {
                        rename(keychain, toUseBuff2);
                        doneCreating = true;
                    }
                    else
                        index++;
                }
                else
                    doneCreating = true;	// failure to get c string.
                CFRelease(newNameCFStr);
            }
            else
                doneCreating = false; // failure to create mutable string.
        }
        else
            doneCreating = false; // failure to get the string (i.e. > MAXPATHLEN?)
    }
    while (!doneCreating && index != INT_MAX);
}

#define KEYCHAIN_SYNC_KEY CFSTR("KeychainSyncList")
#define KEYCHAIN_SYNC_DOMAIN CFSTR("com.apple.keychainsync")

static CFStringRef MakeExpandedPath (const char* path)
{
	std::string name = DLDbListCFPref::ExpandTildesInPath (std::string (path));
	CFStringRef expanded = CFStringCreateWithCString (NULL, name.c_str (), 0);
	return expanded;
}

void StorageManager::removeKeychainFromSyncList (const DLDbIdentifier &id)
{
	// make a CFString of our identifier
	const char* idname = id.dbName ();
	if (idname == NULL)
	{
		return;
	}
	
	CFStringRef idString = MakeExpandedPath (idname);
	
	// check and see if this keychain is in the keychain syncing list
	CFArrayRef value =	 
		(CFArrayRef) CFPreferencesCopyValue (KEYCHAIN_SYNC_KEY,
											 KEYCHAIN_SYNC_DOMAIN,
											 kCFPreferencesCurrentUser,
											 kCFPreferencesAnyHost);
	if (value == NULL)
	{
		return;
	}
	
	// make a mutable copy of the dictionary
	CFMutableArrayRef mtValue = CFArrayCreateMutableCopy (NULL, 0, value);
	CFRelease (value);
	
	// walk the array, looking for the value
	CFIndex i;
	CFIndex limit = CFArrayGetCount (mtValue);
	bool found = false;
	
	for (i = 0; i < limit; ++i)
	{
		CFDictionaryRef idx = (CFDictionaryRef) CFArrayGetValueAtIndex (mtValue, i);
		CFStringRef v = (CFStringRef) CFDictionaryGetValue (idx, CFSTR("DbName"));
		if (v == NULL)
		{
			return; // something is really wrong if this is taken
		}
		
		CFStringRef vExpanded = MakeExpandedPath (CFStringGetCStringPtr (v, 0));
		CFComparisonResult result = CFStringCompare (vExpanded, idString, 0);
		CFRelease (vExpanded);
		
		if (result == 0)
		{
			CFArrayRemoveValueAtIndex (mtValue, i);
			found = true;
			break;
		}
	}
	
	if (found)
	{
		CFShow (mtValue);
		
		CFPreferencesSetValue (KEYCHAIN_SYNC_KEY,
							   mtValue,
							   KEYCHAIN_SYNC_DOMAIN,
							   kCFPreferencesCurrentUser,
							   kCFPreferencesAnyHost);
		CFPreferencesSynchronize (KEYCHAIN_SYNC_DOMAIN, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	}
	
	CFRelease (idString);
	CFRelease (mtValue);
}

void StorageManager::remove(const KeychainList &kcsToRemove, bool deleteDb)
{
	bool unsetDefault = false;
	{
		mSavedList.revert(true);
		DLDbIdentifier defaultId = mSavedList.defaultDLDbIdentifier();
		for (KeychainList::const_iterator ix = kcsToRemove.begin();
			ix != kcsToRemove.end(); ++ix)
		{
			// Find the keychain object for the given ref
			Keychain theKeychain = *ix;
			DLDbIdentifier dLDbIdentifier = theKeychain->dlDbIdentifier();
	
			// Remove it from the saved list
			mSavedList.remove(dLDbIdentifier);
			if (dLDbIdentifier == defaultId)
				unsetDefault=true;

			if (deleteDb)
			{
				removeKeychainFromSyncList (dLDbIdentifier);
				
				// Now remove it from the cache
				removeKeychain(dLDbIdentifier, theKeychain.get());
			}
		}

		if (unsetDefault)
			mSavedList.defaultDLDbIdentifier(DLDbIdentifier());

		mSavedList.save();
	}

	if (deleteDb)
	{
		// Delete the actual databases without holding any locks.
		for (KeychainList::const_iterator ix = kcsToRemove.begin();
			ix != kcsToRemove.end(); ++ix)
		{
			(*ix)->database()->deleteDb();
		}
	}

	// Make sure we are not holding mLock when we post these events.
	KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);

	if (unsetDefault)
		KCEventNotifier::PostKeychainEvent(kSecDefaultChangedEvent);
}

void
StorageManager::getSearchList(KeychainList &keychainList)
{
	if (gServerMode) {
		keychainList.clear();
		return;
	}

    mSavedList.revert(false);
	mCommonList.revert(false);

	// Merge mSavedList, mDynamicList and mCommonList
	DLDbList dLDbList = mSavedList.searchList();
	DLDbList dynamicList = mDynamicList.searchList();
	DLDbList commonList = mCommonList.searchList();
	KeychainList result;
	result.reserve(dLDbList.size() + dynamicList.size() + commonList.size());

	{
		// Only hold globals().apiLock during the cache lookups
		StLock<Mutex> stAPILock(globals().apiLock);

		for (DLDbList::const_iterator it = dynamicList.begin();
			it != dynamicList.end(); ++it)
		{
			result.push_back(keychain(*it));
		}

		for (DLDbList::const_iterator it = dLDbList.begin();
			it != dLDbList.end(); ++it)
		{
			result.push_back(keychain(*it));
		}

		for (DLDbList::const_iterator it = commonList.begin();
			it != commonList.end(); ++it)
		{
			result.push_back(keychain(*it));
		}
	}

	keychainList.swap(result);
}

void
StorageManager::setSearchList(const KeychainList &keychainList)
{
	DLDbList commonList = mCommonList.searchList();

	// Strip out the common list part from the end of the search list.
	KeychainList::const_iterator it_end = keychainList.end();
	DLDbList::const_reverse_iterator end_common = commonList.rend();
	for (DLDbList::const_reverse_iterator it_common = commonList.rbegin(); it_common != end_common; ++it_common)
	{
		// Eliminate common entries from the end of the passed in keychainList.
		if (it_end == keychainList.begin())
			break;

		--it_end;
		if (!((*it_end)->dlDbIdentifier() == *it_common))
		{
			++it_end;
			break;
		}
	}

	/* it_end now points one past the last element in keychainList which is not in commonList. */
	DLDbList searchList, oldSearchList(mSavedList.searchList());
	for (KeychainList::const_iterator it = keychainList.begin(); it != it_end; ++it)
	{
		searchList.push_back((*it)->dlDbIdentifier());
	}

	{
		// Set the current searchlist to be what was passed in, the old list will be freed
		// upon exit of this stackframe.
		mSavedList.revert(true);
		mSavedList.searchList(searchList);
    	mSavedList.save();
	}

	if (!(oldSearchList == searchList))
	{
		// Make sure we are not holding mLock when we post this event.
		KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);
	}
}

void
StorageManager::getSearchList(SecPreferencesDomain domain, KeychainList &keychainList)
{
	if (gServerMode) {
		keychainList.clear();
		return;
	}

	if (domain == kSecPreferencesDomainDynamic)
	{
		convertList(keychainList, mDynamicList.searchList());
	}
	else if (domain == mDomain)
	{
		mSavedList.revert(false);
		convertList(keychainList, mSavedList.searchList());
	}
	else
	{
		convertList(keychainList, DLDbListCFPref(domain).searchList());
	}
}

void StorageManager::forceUserSearchListReread()
{
	mSavedList.forceUserSearchListReread();
}

void
StorageManager::setSearchList(SecPreferencesDomain domain, const KeychainList &keychainList)
{
	if (domain == kSecPreferencesDomainDynamic)
		MacOSError::throwMe(errSecInvalidPrefsDomain);

	DLDbList searchList;
	convertList(searchList, keychainList);

	if (domain == mDomain)
	{
		DLDbList oldSearchList(mSavedList.searchList());
		{
			// Set the current searchlist to be what was passed in, the old list will be freed
			// upon exit of this stackframe.
			mSavedList.revert(true);
			mSavedList.searchList(searchList);
			mSavedList.save();
		}

		if (!(oldSearchList == searchList))
		{
			KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);
		}
	}
	else
	{
		DLDbListCFPref(domain).searchList(searchList);
	}
}

void
StorageManager::domain(SecPreferencesDomain domain)
{
	if (domain == kSecPreferencesDomainDynamic)
		MacOSError::throwMe(errSecInvalidPrefsDomain);

	if (domain == mDomain)
		return;	// no change

#if !defined(NDEBUG)
	switch (domain)
	{
	case kSecPreferencesDomainSystem:
		secdebug("storagemgr", "switching to system domain"); break;
	case kSecPreferencesDomainUser:
		secdebug("storagemgr", "switching to user domain (uid %d)", getuid()); break;
	default:
		secdebug("storagemgr", "switching to weird prefs domain %d", domain); break;
	}
#endif

	mDomain = domain;
	mSavedList.set(domain);
}

void
StorageManager::optionalSearchList(CFTypeRef keychainOrArray, KeychainList &keychainList)
{
	if (!keychainOrArray)
		getSearchList(keychainList);
	else
	{
		CFTypeID typeID = CFGetTypeID(keychainOrArray);
		if (typeID == CFArrayGetTypeID())
			convertToKeychainList(CFArrayRef(keychainOrArray), keychainList);
		else if (typeID == gTypes().KeychainImpl.typeID)
			keychainList.push_back(KeychainImpl::required(SecKeychainRef(keychainOrArray)));
		else
			MacOSError::throwMe(paramErr);
	}
}

// static methods.
void
StorageManager::convertToKeychainList(CFArrayRef keychainArray, KeychainList &keychainList)
{
	assert(keychainArray);
	CFIndex count = CFArrayGetCount(keychainArray);
	KeychainList keychains(count);
	for (CFIndex ix = 0; ix < count; ++ix)
	{
		keychains[ix] = KeychainImpl::required(SecKeychainRef(CFArrayGetValueAtIndex(keychainArray, ix)));
	}

	keychainList.swap(keychains);
}

CFArrayRef
StorageManager::convertFromKeychainList(const KeychainList &keychainList)
{
	CFRef<CFMutableArrayRef> keychainArray(CFArrayCreateMutable(NULL, keychainList.size(), &kCFTypeArrayCallBacks));

	for (KeychainList::const_iterator ix = keychainList.begin(); ix != keychainList.end(); ++ix)
	{
		SecKeychainRef keychainRef = (*ix)->handle();
		CFArrayAppendValue(keychainArray, keychainRef);
		CFRelease(keychainRef);
	}

	// Counter the CFRelease that CFRef<> is about to do when keychainArray goes out of scope.
	CFRetain(keychainArray);
	return keychainArray;
}

void StorageManager::convertList(DLDbList &ids, const KeychainList &kcs)
{
	DLDbList result;
	result.reserve(kcs.size());
	for (KeychainList::const_iterator ix = kcs.begin(); ix != kcs.end(); ++ix)
	{
		result.push_back((*ix)->dlDbIdentifier());
	}
	ids.swap(result);
}

void StorageManager::convertList(KeychainList &kcs, const DLDbList &ids)
{
	KeychainList result;
    result.reserve(ids.size());
	{
		StLock<Mutex> stAPILock(globals().apiLock);
		for (DLDbList::const_iterator ix = ids.begin(); ix != ids.end(); ++ix)
			result.push_back(keychain(*ix));
	}
    kcs.swap(result);
}

#pragma mark ���� Login Functions ����

void StorageManager::login(AuthorizationRef authRef, UInt32 nameLength, const char* name)
{
    AuthorizationItemSet* info = NULL;
    OSStatus result = AuthorizationCopyInfo(authRef, NULL, &info);	// get the results of the copy rights call.
    Boolean created = false;
    if ( result == noErr && info->count )
    {
        // Grab the password from the auth context (info) and create the keychain...
        //
        AuthorizationItem* currItem = info->items;
        for (UInt32 index = 1; index <= info->count; index++) //@@@plugin bug won't return a specific context.
        {
            if (strcmp(currItem->name, kAuthorizationEnvironmentPassword) == 0)
            {
                // creates the login keychain with the specified password
                try
                {
                    login(nameLength, name, currItem->valueLength, currItem->value);
                    created = true;
                }
                catch(...)
                {
                }
                break;
            }
            currItem++;
        }
    }
    if ( info )
        AuthorizationFreeItemSet(info);

    if ( !created )
        MacOSError::throwMe(errAuthorizationInternal);
}

void StorageManager::login(ConstStringPtr name, ConstStringPtr password)
{
    if ( name == NULL || password == NULL )
        MacOSError::throwMe(paramErr);

	login(name[0], name + 1, password[0], password + 1);
}

void StorageManager::login(UInt32 nameLength, const void *name,
	UInt32 passwordLength, const void *password)
{
	x_debug("StorageManager::login: entered");
	if (passwordLength != 0 && password == NULL)
	{
		x_debug("StorageManager::login: invalid argument (NULL password)");
		MacOSError::throwMe(paramErr);
	}

	DLDbIdentifier loginDLDbIdentifier;
	{
		mSavedList.revert(true);
		loginDLDbIdentifier = mSavedList.loginDLDbIdentifier();
	}

	x_debug1("StorageManager::login: loginDLDbIdentifier is %s", (loginDLDbIdentifier) ? loginDLDbIdentifier.dbName() : "<NULL>");
	if (!loginDLDbIdentifier)
		MacOSError::throwMe(errSecNoSuchKeychain);

	
	
	/***** Variables used thoughout the whole Function****************/
		//Extract User ID
		int uid = geteuid();
		//check for root
		
		struct passwd * pw;
		//Search password database for the given user uid
		pw = getpwuid(uid);
		if (pw == NULL){
			x_debug("StorageManager::login: invalid argument (NULL uid)");
			MacOSError::throwMe(paramErr);
		}
			
		//Get user name from the returned passwd struct
		char * userName		= pw->pw_name;
		
		//Creating shortname Full path
		std::string shortnameKeychain = DLDbListCFPref::ExpandTildesInPath(kLoginKeychainPathPrefix);
		shortnameKeychain += userName; // to access, use shortnameKeychain.c_str()
		
		//lKeychain contains the full path of login.keychain
		std::string lKeychain = DLDbListCFPref::ExpandTildesInPath(kLoginKeychainPathPrefix);
		lKeychain += "login.keychain";
		
		//Used to check if the shortnamekeychain exist
		struct stat st;
		int stat_result = stat(shortnameKeychain.c_str(), &st);
	/****************************************************************/
	
	Keychain theKeychain(keychain(loginDLDbIdentifier));
	
	//Flag to determine if the login.keychain unlocked
	//	This variable is used for the "Allowing another keychain other than login.keychain to be unlocked"
	bool loginUnlocked = false;	
	try
	{
		x_debug2("Attempting to unlock login keychain %s with %d-character password", (theKeychain) ? theKeychain->name() : "<NULL>", (unsigned int)passwordLength);
		theKeychain->unlock(CssmData(const_cast<void *>(password), passwordLength));
		// x_debug("Login keychain unlocked successfully");
		loginUnlocked = true;
	}
	catch(const CssmError &e)
	{
		if (e.osStatus() != CSSMERR_DL_DATASTORE_DOESNOT_EXIST)
			throw;
		
		int rename_stat = 0;
		
		//if "~/Libary/Keychains/shortname" file Exist
		//Note: At this point we know that login.keychain is not made yet, therfore
		//if "~/Libary/Keychains/shortname" we rename it to login.keychain
		if(stat_result == 0){
			
			//Rename "~/Libary/Keychains/shortname" to "~/Libary/Keychains/login.keychain"
			rename_stat = ::rename(shortnameKeychain.c_str(), lKeychain.c_str());
			
			if(rename_stat != 0){
				MacOSError::throwMe(errno);
			}
			
			// make the shortname identifier
			CSSM_VERSION version = {0, 0};
			DLDbIdentifier shortnameDLDbIdentifier = DLDbListCFPref::makeDLDbIdentifier(gGuidAppleCSPDL, version, 0, CSSM_SERVICE_CSP | CSSM_SERVICE_DL,shortnameKeychain.c_str(),NULL);
			
			//if the short keychain exists and it is the only item in
			//the plist, rename it and blow the plist away.
			if (mSavedList.searchList().size() == 1 && mSavedList.member(shortnameDLDbIdentifier)){
				mSavedList.remove(shortnameDLDbIdentifier);
			}
			//If the shortname is in the plist, rename it in the plist as well
			else if(mSavedList.member(shortnameDLDbIdentifier)){
				mSavedList.revert(true);
				
				// theKeychain is in the searchList so lets rename it
				mSavedList.rename(shortnameDLDbIdentifier,loginDLDbIdentifier);
				mSavedList.save();
			}
			
		}else{
			//Default login.keychain is created	
			x_debug1("Creating login keychain %s", (loginDLDbIdentifier) ? loginDLDbIdentifier.dbName() : "<NULL>");
			theKeychain->create(passwordLength, password);
			x_debug("Login keychain created successfully");
			// Set the prefs for this new login keychain.
			loginKeychain(theKeychain);
			// Login Keychain does not lock on sleep nor lock after timeout by default.
			theKeychain->setSettings(INT_MAX, false);
		}
	}
	
	/********** This part of the Code is a Temp solution for when a User wants 
	*			 another Keychain (other than login.keychain) to auto unlock    *************************************
	*
	*Description: 
	*	If the shortname keychain exist and is not in the searchlist then we added it. Also this code
	*	now will automatically unlock any shortname keychain that has the same password as login.keychain.
	****************************************************************************************************************/

	
	try{
		
		//If the shrtnamekeychain is not in the searchlist then we should add it to the SearchList
		CSSM_VERSION version = {0, 0};
		DLDbIdentifier shrtnameDLDbIdentifier = DLDbListCFPref::makeDLDbIdentifier(gGuidAppleCSPDL, version, 0, CSSM_SERVICE_CSP | CSSM_SERVICE_DL,shortnameKeychain.c_str(),NULL);
		if( !(mSavedList.member(shrtnameDLDbIdentifier))  && (stat_result == 0) ){
			// the Keychain exists and is not in our search list, so add it to the 
			// search list.
			mSavedList.revert(true);
			mSavedList.add(shrtnameDLDbIdentifier);
			mSavedList.save();
		}
		
		Keychain shrtnameKC(keychain(shrtnameDLDbIdentifier));
					
		
		x_debug2("Attempting to unlock shrtnamekeychain %s with %d-character password", 
			(shrtnameKC) ? shrtnameKC->name() : "<NULL>", (unsigned int)passwordLength);
		shrtnameKC->unlock(CssmData(const_cast<void *>(password), passwordLength));
	}
	catch(const CssmError &e){
		if (e.osStatus() != CSSMERR_DL_DATASTORE_DOESNOT_EXIST)
			throw;
	}
	//***************************************************************************************************************
	
}

void StorageManager::logout()
{
    // nothing left to do here
}

void StorageManager::changeLoginPassword(ConstStringPtr oldPassword, ConstStringPtr newPassword)
{
	loginKeychain()->changePassphrase(oldPassword, newPassword);
	secdebug("KClogin", "Changed login keychain password successfully");
}


void StorageManager::changeLoginPassword(UInt32 oldPasswordLength, const void *oldPassword,  UInt32 newPasswordLength, const void *newPassword)
{
	loginKeychain()->changePassphrase(oldPasswordLength, oldPassword,  newPasswordLength, newPassword);
	secdebug("KClogin", "Changed login keychain password successfully");
}

// Clear out the keychain search list and rename the existing login.keychain.
//
void StorageManager::resetKeychain(Boolean resetSearchList)
{
    // Clear the keychain search list.
    try
    {
        if ( resetSearchList )
        {
            StorageManager::KeychainList keychainList;
            setSearchList(keychainList);
        }
        // Get a reference to the existing login keychain...
        // If we don't have one, we throw (not requiring a rename).
        //
        Keychain keychain = loginKeychain();
        //
        // Rename the existing login.keychain (i.e. put it aside).
        //
        CFMutableStringRef newName = NULL;
        newName = CFStringCreateMutable(NULL, 0);
        CFStringRef currName = NULL;
        currName = CFStringCreateWithCString(NULL, keychain->name(), kCFStringEncodingUTF8);
        if ( newName && currName )
        {
            CFStringAppend(newName, currName);
            CFStringRef kcSuffix = CFSTR(kKeychainSuffix);
            if ( CFStringHasSuffix(newName, kcSuffix) )	// remove the .keychain extension
            {
                CFRange suffixRange = CFStringFind(newName, kcSuffix, 0);
                CFStringFindAndReplace(newName, kcSuffix, CFSTR(""), suffixRange, 0);
            }
            CFStringAppend(newName, CFSTR(kKeychainRenamedSuffix));	// add "_renamed"
            try
            {
                renameUnique(keychain, newName);
            }
            catch(...)
            {
                // we need to release 'newName' & 'currName'
            }
        }	 // else, let the login call report a duplicate
        if ( newName )
            CFRelease(newName);
        if ( currName )
            CFRelease(currName);
    }
    catch(...)
    {
        // We either don't have a login keychain, or there was a
        // failure to rename the existing one.
    }
}

#pragma mark ���� File Related ����

Keychain StorageManager::make(const char *pathName)
{
	return make(pathName, true);
}

Keychain StorageManager::make(const char *pathName, bool add)
{
	string fullPathName;
    if ( pathName[0] == '/' )
		fullPathName = pathName;
	else
    {
		// Get Home directory from environment.
		switch (mDomain)
		{
		case kSecPreferencesDomainUser:
			{
				const char *homeDir = getenv("HOME");
				if (homeDir == NULL)
				{
					// If $HOME is unset get the current user's home directory
					// from the passwd file.
					uid_t uid = geteuid();
					if (!uid) uid = getuid();
					struct passwd *pw = getpwuid(uid);
					if (!pw)
						MacOSError::throwMe(paramErr);
					homeDir = pw->pw_dir;
				}
				fullPathName = homeDir;
			}
			break;
		case kSecPreferencesDomainSystem:
			fullPathName = "";
			break;
		default:
			assert(false);	// invalid domain for this
		}

		fullPathName += "/Library/Keychains/";
		fullPathName += pathName;
	}

    const CSSM_NET_ADDRESS *DbLocation = NULL;	// NULL for keychains
    const CSSM_VERSION *version = NULL;
    uint32 subserviceId = 0;
    CSSM_SERVICE_TYPE subserviceType = CSSM_SERVICE_DL | CSSM_SERVICE_CSP;
    const CssmSubserviceUid ssuid(gGuidAppleCSPDL, version,
                                   subserviceId, subserviceType);
	DLDbIdentifier dLDbIdentifier(ssuid, fullPathName.c_str(), DbLocation);
	return makeKeychain(dLDbIdentifier, add);
}

Keychain StorageManager::makeLoginAuthUI(const Item *item)
{
    // Create a login/default keychain for the user using UI.
    // The user can cancel out of the operation, or create a new login keychain.
    // If auto-login is turned off, the user will be asked for their login password.
    //
    OSStatus result = noErr;
    Keychain keychain;	// We return this keychain.
    //
    // Set up the Auth ref to bring up UI.
    //
	AuthorizationItem *currItem, *authEnvirItemArrayPtr = NULL;
    AuthorizationRef authRef = NULL;
	try
	{
		result = AuthorizationCreate(NULL, NULL, kAuthorizationFlagDefaults, &authRef);
		if ( result )
			MacOSError::throwMe(result);

		AuthorizationEnvironment envir;
		envir.count = 6;	// up to 6 hints can be used.
		authEnvirItemArrayPtr = (AuthorizationItem*)malloc(sizeof(AuthorizationItem) * envir.count);
		if ( !authEnvirItemArrayPtr )
			MacOSError::throwMe(errAuthorizationInternal);

		currItem = envir.items = authEnvirItemArrayPtr;
		
		//
		// 1st Hint (optional): The keychain item's account attribute string. 
		//						When item is specified, we assume an 'add' operation is being attempted.
		char buff[255];
		UInt32 actLen = 0;
		SecKeychainAttribute attr = { kSecAccountItemAttr, 255, &buff };
		if ( item )
		{
			try
			{
				(*item)->getAttribute(attr, &actLen);
			}
			catch(...)
			{
				actLen = 0;	// This item didn't have the account attribute, so don't display one in the UI.
			}
		}
		currItem->name = AGENT_HINT_ATTR_NAME;	// name str that identifies this hint as attr name
		if ( actLen )	// Fill in the hint if we have an account attr
		{
			if ( actLen > 255 )
				buff[255] = 0;
			else
				buff[actLen] = 0;
			currItem->valueLength = strlen(buff)+1;
			currItem->value = buff;
		}
		else
		{
			currItem->valueLength = 0;
			currItem->value = NULL;
		}
		currItem->flags = 0;

		//
		// 2nd Hint (optional): The item's keychain full path.
		//
		currItem++;
		char* currDefaultName = NULL;
		try
		{
			currDefaultName = (char*)defaultKeychain()->name();	// Use the name if we have it.
			currItem->name = AGENT_HINT_LOGIN_KC_NAME;	// Name str that identifies this hint as kc path
			currItem->valueLength = (currDefaultName) ? strlen(currDefaultName) : 0;
			currItem->value = (currDefaultName) ? (void*)currDefaultName : (void*)"";
			currItem->flags = 0;
			currItem++;
		}
		catch(...)
		{
			envir.count--;
		}
		
		//
		// 3rd Hint (required): check if curr default keychain is unavailable.
		// This is determined by the parent not existing.
		//
		currItem->name = AGENT_HINT_LOGIN_KC_EXISTS_IN_KC_FOLDER;
		Boolean loginUnavail = false;
		try
		{
			Keychain defaultKC = defaultKeychain();
			if ( !defaultKC->exists() )
				loginUnavail = true;
		}
		catch(...)	// login.keychain not present
		{
		}
		currItem->valueLength = sizeof(Boolean);
		currItem->value = (void*)&loginUnavail;
		currItem->flags = 0;

		//
		// 4th Hint (required): userName
		//
		currItem++;
		currItem->name = AGENT_HINT_LOGIN_KC_USER_NAME;
		char* uName = getenv("USER");
		string userName = uName ? uName : "";
		if ( userName.length() == 0 )
		{
			uid_t uid = geteuid();
			if (!uid) uid = getuid();
			struct passwd *pw = getpwuid(uid);	// fallback case...
			if (pw)
				userName = pw->pw_name;
			endpwent();
		}
		if ( userName.length() == 0 )	// did we ultimately get one?
			MacOSError::throwMe(errAuthorizationInternal);

		currItem->value = (void*)userName.c_str();
		currItem->valueLength = userName.length();
		currItem->flags = 0;

		//
		// 5th Hint (required): flags if user has more than 1 keychain (used for a later warning when reset to default).
		//
		currItem++;
		currItem->name = AGENT_HINT_LOGIN_KC_USER_HAS_OTHER_KCS_STR;
		Boolean moreThanOneKCExists = false;
		{
			// if item is NULL, then this is a user-initiated full reset
			if (item && mSavedList.searchList().size() > 1)
				moreThanOneKCExists = true;
		}
		currItem->value = &moreThanOneKCExists;
		currItem->valueLength = sizeof(Boolean);
		currItem->flags = 0;

		//
		// 6th Hint (required): If no item is involved, this is a user-initiated full reset.
		// We want to suppress the "do you want to reset to defaults?" panel in this case.
		//
		currItem++;
		currItem->name = AGENT_HINT_LOGIN_KC_SUPPRESS_RESET_PANEL;
		Boolean suppressResetPanel = (item == NULL) ? TRUE : FALSE;
		currItem->valueLength = sizeof(Boolean);
		currItem->value = (void*)&suppressResetPanel;
		currItem->flags = 0;

		//
		// Set up the auth rights and make the auth call.
		//
		AuthorizationItem authItem = { LOGIN_KC_CREATION_RIGHT, 0 , NULL, 0 };
		AuthorizationRights rights = { 1, &authItem };
		AuthorizationFlags flags = kAuthorizationFlagDefaults | kAuthorizationFlagInteractionAllowed | kAuthorizationFlagExtendRights;
		result = AuthorizationCopyRights(authRef, &rights, &envir, flags, NULL);
		if ( result )
			MacOSError::throwMe(result);
		try
		{
			resetKeychain(true); // Clears the plist, moves aside existing login.keychain
		}
		catch (...) // can throw if no existing login.keychain is found
		{
		}
		login(authRef, userName.length(), userName.c_str()); // Create login.keychain
		keychain = loginKeychain(); // Get newly-created login keychain
		defaultKeychain(keychain);	// Set it to be the default

		free(authEnvirItemArrayPtr);
		AuthorizationFree(authRef, kAuthorizationFlagDefaults);
	}
	
	catch (...)
	{
		// clean up allocations, then rethrow error
		if ( authEnvirItemArrayPtr )
			free(authEnvirItemArrayPtr);
		if ( authRef )
			AuthorizationFree(authRef, kAuthorizationFlagDefaults);
		throw;
	}

    return keychain;
}

Keychain StorageManager::defaultKeychainUI(Item &item)
{
    Keychain returnedKeychain;
    try
    {
        returnedKeychain = defaultKeychain(); // If we have one, return it.
        if ( returnedKeychain->exists() )
            return returnedKeychain;
    }
    catch(...)	// We could have one, but it isn't available (i.e. on a un-mounted volume).
    {
    }
    if ( globals().getUserInteractionAllowed() )
    {
        returnedKeychain = makeLoginAuthUI(&item); // If no Keychains is present, one will be created.
        if ( !returnedKeychain )
            MacOSError::throwMe(errSecInvalidKeychain);	// Something went wrong...
    }
    else
        MacOSError::throwMe(errSecInteractionNotAllowed); // If UI isn't allowed, return an error.

    return returnedKeychain;
}

void
StorageManager::addToDomainList(SecPreferencesDomain domain,
	const char* dbName, const CSSM_GUID &guid, uint32 subServiceType)
{
	if (domain == kSecPreferencesDomainDynamic)
		MacOSError::throwMe(errSecInvalidPrefsDomain);

	// make the identifier
	CSSM_VERSION version = {0, 0};
	DLDbIdentifier id = DLDbListCFPref::makeDLDbIdentifier (guid, version, 0,
		subServiceType, dbName, NULL);

	if (domain == mDomain)
	{
		// manipulate the user's list
		{
			mSavedList.revert(true);
			mSavedList.add(id);
			mSavedList.save();
		}

		KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);
	}
	else
	{
		// manipulate the other list
		DLDbListCFPref(domain).add(id);
	}
}

void
StorageManager::isInDomainList(SecPreferencesDomain domain,
	const char* dbName, const CSSM_GUID &guid, uint32 subServiceType)
{
	if (domain == kSecPreferencesDomainDynamic)
		MacOSError::throwMe(errSecInvalidPrefsDomain);

	CSSM_VERSION version = {0, 0};
	DLDbIdentifier id = DLDbListCFPref::makeDLDbIdentifier (guid, version, 0,
		subServiceType, dbName, NULL);

	// determine the list to search
	bool result;
	if (domain == mDomain)
	{
		result = mSavedList.member(id);
	}
	else
	{
		result = DLDbListCFPref(domain).member(id);
	}
	
	// do the search
	if (!result)
	{
		MacOSError::throwMe(errSecNoSuchKeychain);
	}
}

void
StorageManager::removeFromDomainList(SecPreferencesDomain domain,
	const char* dbName, const CSSM_GUID &guid, uint32 subServiceType)
{
	if (domain == kSecPreferencesDomainDynamic)
		MacOSError::throwMe(errSecInvalidPrefsDomain);

	// make the identifier
	CSSM_VERSION version = {0, 0};
	DLDbIdentifier id = DLDbListCFPref::makeDLDbIdentifier (guid, version, 0,
		subServiceType, dbName, NULL);

	if (domain == mDomain)
	{
		// manipulate the user's list
		{
			mSavedList.revert(true);
			mSavedList.remove(id);
			mSavedList.save();
		}

		KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);
	}
	else
	{
		// manipulate the other list
		DLDbListCFPref(domain).remove(id);
	}
}

