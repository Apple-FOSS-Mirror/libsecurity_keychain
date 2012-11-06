/*
 * Copyright (c) 2002-2004 Apple Computer, Inc. All Rights Reserved.
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

#include <Security/SecIdentity.h>
#include <Security/SecIdentityPriv.h>
#include <Security/SecKeychainItemPriv.h>

#include "SecBridge.h"
#include <security_keychain/Certificate.h>
#include <security_keychain/Identity.h>
#include <security_keychain/KeyItem.h>
#include <security_keychain/KCCursor.h>
#include <security_cdsa_utilities/Schema.h>
#include <security_utilities/simpleprefs.h>
#include <sys/param.h>
#include <syslog.h>


CFTypeID
SecIdentityGetTypeID(void)
{
	BEGIN_SECAPI

	return gTypes().Identity.typeID;

	END_SECAPI1(_kCFRuntimeNotATypeID)
}


OSStatus
SecIdentityCopyCertificate(
            SecIdentityRef identityRef, 
            SecCertificateRef *certificateRef)
{
    BEGIN_SECAPI

	SecPointer<Certificate> certificatePtr(Identity::required(identityRef)->certificate());
	Required(certificateRef) = certificatePtr->handle();

    END_SECAPI2("SecIdentityCopyCertificate")
}


OSStatus
SecIdentityCopyPrivateKey(
            SecIdentityRef identityRef, 
            SecKeyRef *privateKeyRef)
{
    BEGIN_SECAPI

	SecPointer<KeyItem> keyItemPtr(Identity::required(identityRef)->privateKey());
	Required(privateKeyRef) = keyItemPtr->handle();

    END_SECAPI2("SecIdentityCopyPrivateKey")
}

OSStatus
SecIdentityCreateWithCertificate(
	CFTypeRef keychainOrArray,
	SecCertificateRef certificateRef,
	SecIdentityRef *identityRef)
{
    BEGIN_SECAPI

	SecPointer<Certificate> certificatePtr(Certificate::required(certificateRef));
	StorageManager::KeychainList keychains;
	globals().storageManager.optionalSearchList(keychainOrArray, keychains);
	SecPointer<Identity> identityPtr(new Identity(keychains, certificatePtr));
	Required(identityRef) = identityPtr->handle();

    END_SECAPI2("SecIdentityCreateWithCertificate")
}

CFComparisonResult
SecIdentityCompare(
	SecIdentityRef identity1,
	SecIdentityRef identity2,
	CFOptionFlags compareOptions)
{
	if (!identity1 || !identity2)
	{
		if (identity1 == identity2)
			return kCFCompareEqualTo;
		else if (identity1 < identity2)
			return kCFCompareLessThan;
		else
			return kCFCompareGreaterThan;
	}

	BEGIN_SECAPI

	SecPointer<Identity> id1(Identity::required(identity1));
	SecPointer<Identity> id2(Identity::required(identity2));

	if (id1 == id2)
		return kCFCompareEqualTo;
	else if (id1 < id2)
		return kCFCompareLessThan;
	else
		return kCFCompareGreaterThan;

	END_SECAPI1(kCFCompareGreaterThan);
}

CFArrayRef _SecIdentityCopyPossiblePaths(
    CFStringRef name)
{
    // utility function to build and return an array of possible paths for the given name.
    // if name is not a URL, this returns a single-element array.
    // if name is a URL, the array may contain 1-N elements, one for each level of the path hierarchy.
    
    CFMutableArrayRef names = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!name) {
        return names;
    }
    CFIndex oldLength = CFStringGetLength(name);
    CFArrayAppendValue(names, name);

    CFURLRef url = CFURLCreateWithString(NULL, name, NULL);
    if (url && CFURLCanBeDecomposed(url)) {
        // first, remove the query portion of this URL, if any
        CFStringRef qs = CFURLCopyQueryString(url, NULL);
        if (qs) {
            CFMutableStringRef newName = CFStringCreateMutableCopy(NULL, oldLength, name);
            if (newName) {
                CFIndex qsLength = CFStringGetLength(qs) + 1; // include the '?'
                CFStringDelete(newName, CFRangeMake(oldLength-qsLength, qsLength));
                CFRelease(url);
                url = CFURLCreateWithString(NULL, newName, NULL);
                CFArraySetValueAtIndex(names, 0, newName);
                CFRelease(newName);
            }
            CFRelease(qs);
        }
        // now add an entry for each level of the path
        while (url) {
            CFURLRef parent = CFURLCreateCopyDeletingLastPathComponent(NULL, url);
            if (parent) {
                CFStringRef parentURLString = CFURLGetString(parent);
                if (parentURLString) {
                    CFIndex newLength = CFStringGetLength(parentURLString);
                    // check that string length has decreased as expected; for file URLs,
                    // CFURLCreateCopyDeletingLastPathComponent can insert './' or '../'
                    if ((newLength >= oldLength) || (!CFStringHasPrefix(name, parentURLString))) {
                        CFRelease(parent);
                        CFRelease(url);
                        break;
                    }
                    oldLength = newLength;
                    CFArrayAppendValue(names, parentURLString);
                }
            }
            CFRelease(url);
            url = parent;
        }
    }
    
    return names;
}

OSStatus _SecIdentityCopyPreferenceMatchingName(
    CFStringRef name,
    CSSM_KEYUSE keyUsage,
    CFArrayRef validIssuers,
    SecIdentityRef *identity)
{
    // this is NOT exported, and called only from SecIdentityCopyPreference (below), so no BEGIN/END macros here;
    // caller must handle exceptions

	StorageManager::KeychainList keychains;
	globals().storageManager.getSearchList(keychains);
	KCCursor cursor(keychains, kSecGenericPasswordItemClass, NULL);

	char idUTF8[MAXPATHLEN];
    Required(name);
    if (!CFStringGetCString(name, idUTF8, sizeof(idUTF8)-1, kCFStringEncodingUTF8))
        idUTF8[0] = (char)'\0';
    CssmData service(const_cast<char *>(idUTF8), strlen(idUTF8));
    cursor->add(CSSM_DB_EQUAL, Schema::attributeInfo(kSecServiceItemAttr), service);
	cursor->add(CSSM_DB_EQUAL, Schema::attributeInfo(kSecTypeItemAttr), (FourCharCode)'iprf');
    if (keyUsage)
        cursor->add(CSSM_DB_EQUAL, Schema::attributeInfo(kSecScriptCodeItemAttr), (sint32)keyUsage);

	Item prefItem;
	if (!cursor->next(prefItem))
		return errSecItemNotFound;

	// get persistent certificate reference
	SecKeychainAttribute itemAttrs[] = { { kSecGenericItemAttr, 0, NULL } };
	SecKeychainAttributeList itemAttrList = { sizeof(itemAttrs) / sizeof(itemAttrs[0]), itemAttrs };
	prefItem->getContent(NULL, &itemAttrList, NULL, NULL);

	// find certificate, given persistent reference data
	CFDataRef pItemRef = CFDataCreateWithBytesNoCopy(NULL, (const UInt8 *)itemAttrs[0].data, itemAttrs[0].length, kCFAllocatorNull);
	SecKeychainItemRef certItemRef = nil;
	OSStatus status = SecKeychainItemCopyFromPersistentReference(pItemRef, &certItemRef); //%%% need to make this a method of ItemImpl
	prefItem->freeContent(&itemAttrList, NULL);
	if (pItemRef)
		CFRelease(pItemRef);
	if (status)
		return status;
    
    // filter on valid issuers, if provided
    if (validIssuers) {
        //%%%TBI
    }

	// create identity reference, given certificate
	Item certItem = ItemImpl::required(SecKeychainItemRef(certItemRef));
	SecPointer<Certificate> certificate(static_cast<Certificate *>(certItem.get()));
	SecPointer<Identity> identity_ptr(new Identity(keychains, certificate));
	if (certItemRef)
		CFRelease(certItemRef);

	Required(identity) = identity_ptr->handle();

    return status;
}

OSStatus SecIdentityCopyPreference(
    CFStringRef name,
    CSSM_KEYUSE keyUsage,
    CFArrayRef validIssuers,
    SecIdentityRef *identity)
{
    // The original implementation of SecIdentityCopyPreference matches the exact string only.
    // That implementation has been moved to _SecIdentityCopyPreferenceMatchingName (above),
    // and this function is a wrapper which calls it, so that existing clients will get the
    // extended behavior of server domain matching for items that specify URLs.
    // (Note that behavior is unchanged if the specified name is not a URL.)

    BEGIN_SECAPI

    CFTypeRef val = (CFTypeRef)CFPreferencesCopyValue(CFSTR("LogIdentityPreferenceLookup"),
                    CFSTR("com.apple.security"),
                    kCFPreferencesCurrentUser,
                    kCFPreferencesAnyHost);
    Boolean logging = false;
    if (val && CFGetTypeID(val) == CFBooleanGetTypeID()) {
        logging = CFBooleanGetValue((CFBooleanRef)val);
        CFRelease(val);
    }

    OSStatus status = errSecItemNotFound;
    CFArrayRef names = _SecIdentityCopyPossiblePaths(name);
    if (!names) {
        return status;
    }

    CFIndex idx, total = CFArrayGetCount(names);
    for (idx = 0; idx < total; idx++) {
        CFStringRef aName = (CFStringRef)CFArrayGetValueAtIndex(names, idx);
        try {
            status = _SecIdentityCopyPreferenceMatchingName(aName, keyUsage, validIssuers, identity);
        }
        catch (...) { status = errSecItemNotFound; }

        if (logging) {
            // get identity label
            CFStringRef labelString = NULL;
            if (!status && identity && *identity) {
                try {
                    SecPointer<Certificate> cert(Identity::required(*identity)->certificate());
                    cert->inferLabel(false, &labelString);
                }
                catch (...) { labelString = NULL; };
            }
            char *labelBuf = NULL;
            CFIndex labelBufSize = (labelString) ? CFStringGetLength(labelString) * 4 : 4;
            labelBuf = (char *)malloc(labelBufSize);
            if (!labelString || !CFStringGetCString(labelString, labelBuf, labelBufSize, kCFStringEncodingUTF8)) {
                labelBuf[0] = 0;
            }
            if (labelString) {
                CFRelease(labelString);
            }

            // get service name
            char *serviceBuf = NULL;
            CFIndex serviceBufSize = CFStringGetLength(aName) * 4;
            serviceBuf = (char *)malloc(serviceBufSize);
            if (!CFStringGetCString(aName, serviceBuf, serviceBufSize, kCFStringEncodingUTF8)) {
                serviceBuf[0] = 0;
            }

            syslog(LOG_NOTICE, "preferred identity: \"%s\" found for \"%s\"\n", labelBuf, serviceBuf);
            if (!status && name) {
                char *nameBuf = NULL;
                CFIndex nameBufSize = CFStringGetLength(name) * 4;
                nameBuf = (char *)malloc(nameBufSize);
                if (!CFStringGetCString(name, nameBuf, nameBufSize, kCFStringEncodingUTF8)) {
                    nameBuf[0] = 0;
                }
                syslog(LOG_NOTICE, "lookup complete; will use: \"%s\" for \"%s\"\n", labelBuf, nameBuf);
                free(nameBuf);
            }
            
            free(labelBuf);
            free(serviceBuf);
        }

        if (status == noErr) {
            break; // match found
        }
    }

    CFRelease(names);
    return status;

    END_SECAPI2("SecIdentityCopyPreference")
}

OSStatus SecIdentitySetPreference(
    SecIdentityRef identity,
    CFStringRef name,
    CSSM_KEYUSE keyUsage)
{
    BEGIN_SECAPI

	if (!identity || !name)
		MacOSError::throwMe(paramErr);
	SecPointer<Certificate> certificate(Identity::required(identity)->certificate());

    // first look for existing preference, in case this is an update
	StorageManager::KeychainList keychains;
	globals().storageManager.getSearchList(keychains);
	KCCursor cursor(keychains, kSecGenericPasswordItemClass, NULL);

	char idUTF8[MAXPATHLEN];
    if (!CFStringGetCString(name, idUTF8, sizeof(idUTF8)-1, kCFStringEncodingUTF8))
        idUTF8[0] = (char)'\0';
    CssmData service(const_cast<char *>(idUTF8), strlen(idUTF8));
    cursor->add(CSSM_DB_EQUAL, Schema::attributeInfo(kSecServiceItemAttr), service);
	cursor->add(CSSM_DB_EQUAL, Schema::attributeInfo(kSecTypeItemAttr), (FourCharCode)'iprf');
    if (keyUsage)
        cursor->add(CSSM_DB_EQUAL, Schema::attributeInfo(kSecScriptCodeItemAttr), (sint32)keyUsage);

	Item item(kSecGenericPasswordItemClass, 'aapl', 0, NULL, false);
    bool add = (!cursor->next(item));

    // service (use provided string)
    item->setAttribute(Schema::attributeInfo(kSecServiceItemAttr), service);

    // label (use service string as default label)
    item->setAttribute(Schema::attributeInfo(kSecLabelItemAttr), service);

    // type
    item->setAttribute(Schema::attributeInfo(kSecTypeItemAttr), (FourCharCode)'iprf');

    // account (use label of certificate)
    CFStringRef labelString = nil;
	certificate->inferLabel(false, &labelString);
    if (!labelString || !CFStringGetCString(labelString, idUTF8, sizeof(idUTF8)-1, kCFStringEncodingUTF8))
        MacOSError::throwMe(errSecDataTooLarge);
    CssmData account(const_cast<void *>(reinterpret_cast<const void *>(idUTF8)), strlen(idUTF8));
    item->setAttribute(Schema::attributeInfo(kSecAccountItemAttr), account);
    CFRelease(labelString);

    // key usage (overload script code)
    if (keyUsage)
        item->setAttribute(Schema::attributeInfo(kSecScriptCodeItemAttr), (sint32)keyUsage);
    
	// generic attribute (store persistent certificate reference)
	CFDataRef pItemRef = nil;
    certificate->copyPersistentReference(pItemRef);
	if (!pItemRef) {
		MacOSError::throwMe(errSecInvalidItemRef);
    }
	const UInt8 *dataPtr = CFDataGetBytePtr(pItemRef);
	CFIndex dataLen = CFDataGetLength(pItemRef);
	CssmData pref(const_cast<void *>(reinterpret_cast<const void *>(dataPtr)), dataLen);
	item->setAttribute(Schema::attributeInfo(kSecGenericItemAttr), pref);
	CFRelease(pItemRef);

    if (add) {
        Keychain keychain = nil;
        try {
            keychain = globals().storageManager.defaultKeychain();
            if (!keychain->exists())
                MacOSError::throwMe(errSecNoSuchKeychain);	// Might be deleted or not available at this time.
        }
        catch(...) {
            keychain = globals().storageManager.defaultKeychainUI(item);
        }

        keychain->add(item);
    }
	item->update();

    END_SECAPI2("SecIdentitySetPreference")
}

OSStatus
SecIdentityFindPreferenceItem(
	CFTypeRef keychainOrArray,
	CFStringRef idString,
	SecKeychainItemRef *itemRef)
{
    BEGIN_SECAPI

	StorageManager::KeychainList keychains;
	globals().storageManager.optionalSearchList(keychainOrArray, keychains);
	KCCursor cursor(keychains, kSecGenericPasswordItemClass, NULL);

	char idUTF8[MAXPATHLEN];
	if (idString)
	{
		if (!CFStringGetCString(idString, idUTF8, sizeof(idUTF8)-1, kCFStringEncodingUTF8))
			idUTF8[0] = (char)'\0';
		CssmData service(const_cast<char *>(idUTF8), strlen(idUTF8));
		cursor->add(CSSM_DB_EQUAL, Schema::attributeInfo(kSecServiceItemAttr), service);
	}
	cursor->add(CSSM_DB_EQUAL, Schema::attributeInfo(kSecTypeItemAttr), (FourCharCode)'iprf');

	Item item;
	if (!cursor->next(item))
		MacOSError::throwMe(errSecItemNotFound);

	if (itemRef)
		*itemRef=item->handle();

    END_SECAPI2("SecIdentityFindPreferenceItem")
}

OSStatus _SecIdentityAddPreferenceItemWithName(
	SecKeychainRef keychainRef,
	SecIdentityRef identityRef,
	CFStringRef idString,
	SecKeychainItemRef *itemRef)
{
    // this is NOT exported, and called only from SecIdentityAddPreferenceItem (below), so no BEGIN/END macros here;
    // caller must handle exceptions

	if (!identityRef || !idString)
		return paramErr;
	SecPointer<Certificate> cert(Identity::required(identityRef)->certificate());
	Item item(kSecGenericPasswordItemClass, 'aapl', 0, NULL, false);

	char idUTF8[MAXPATHLEN];
	if (!CFStringGetCString(idString, idUTF8, sizeof(idUTF8)-1, kCFStringEncodingUTF8))
		return errSecDataTooLarge;

	// service (use provided string)
	CssmData service(const_cast<void *>(reinterpret_cast<const void *>(idUTF8)), strlen(idUTF8));
	item->setAttribute(Schema::attributeInfo(kSecServiceItemAttr), service);

	// label (use service string as default label)
	item->setAttribute(Schema::attributeInfo(kSecLabelItemAttr), service);

	// type
	item->setAttribute(Schema::attributeInfo(kSecTypeItemAttr), (FourCharCode)'iprf');

	// account (use label of certificate)
	CFStringRef labelString = nil;
	cert->inferLabel(false, &labelString);
	if (!labelString || !CFStringGetCString(labelString, idUTF8, sizeof(idUTF8)-1, kCFStringEncodingUTF8))
		return errSecDataTooLarge;
	CssmData account(const_cast<void *>(reinterpret_cast<const void *>(idUTF8)), strlen(idUTF8));
	item->setAttribute(Schema::attributeInfo(kSecAccountItemAttr), account);
	CFRelease(labelString);

	// generic attribute (store persistent certificate reference)
	CFDataRef pItemRef = nil;
	OSStatus status = SecKeychainItemCreatePersistentReference((SecKeychainItemRef)cert->handle(), &pItemRef);
	if (!pItemRef)
		status = errSecInvalidItemRef;
	if (status)
		 return status;
	const UInt8 *dataPtr = CFDataGetBytePtr(pItemRef);
	CFIndex dataLen = CFDataGetLength(pItemRef);
	CssmData pref(const_cast<void *>(reinterpret_cast<const void *>(dataPtr)), dataLen);
	item->setAttribute(Schema::attributeInfo(kSecGenericItemAttr), pref);
	CFRelease(pItemRef);

	Keychain keychain = nil;
	try {
        keychain = Keychain::optional(keychainRef);
        if (!keychain->exists())
            MacOSError::throwMe(errSecNoSuchKeychain);	// Might be deleted or not available at this time.
    }
    catch(...) {
        keychain = globals().storageManager.defaultKeychainUI(item);
    }

    keychain->add(item);
	item->update();

    if (itemRef)
		*itemRef = item->handle();

    return status;
}

OSStatus SecIdentityAddPreferenceItem(
	SecKeychainRef keychainRef,
	SecIdentityRef identityRef,
	CFStringRef idString,
	SecKeychainItemRef *itemRef)
{
    // The original implementation of SecIdentityAddPreferenceItem adds the exact string only.
    // That implementation has been moved to _SecIdentityAddPreferenceItemWithName (above),
    // and this function is a wrapper which calls it, so that existing clients will get the
    // extended behavior of server domain matching for items that specify URLs.
    // (Note that behavior is unchanged if the specified idString is not a URL.)

    BEGIN_SECAPI

    OSStatus status = internalComponentErr;
    CFArrayRef names = _SecIdentityCopyPossiblePaths(idString);
    if (!names) {
        return status;
    }

    CFIndex total = CFArrayGetCount(names);
    if (total > 0) {
        // add item for name (first element in array)
        CFStringRef aName = (CFStringRef)CFArrayGetValueAtIndex(names, 0);
        try {
            status = _SecIdentityAddPreferenceItemWithName(keychainRef, identityRef, aName, itemRef);
        }
        catch (const MacOSError &err)   { status=err.osStatus(); }
        catch (const CommonError &err)  { status=SecKeychainErrFromOSStatus(err.osStatus()); }
        catch (const std::bad_alloc &)  { status=memFullErr; }
        catch (...)                     { status=internalComponentErr; }
    }
    if (total > 1) {
        // add item for top path level (last element in array)
        OSStatus tmpStatus = noErr;
        CFStringRef aName = (CFStringRef)CFArrayGetValueAtIndex(names, total-1);
        try {
            tmpStatus = _SecIdentityAddPreferenceItemWithName(keychainRef, identityRef, aName, itemRef);
        }
        catch (const MacOSError &err)   { tmpStatus=err.osStatus(); }
        catch (const CommonError &err)  { tmpStatus=SecKeychainErrFromOSStatus(err.osStatus()); }
        catch (const std::bad_alloc &)  { tmpStatus=memFullErr; }
        catch (...)                     { tmpStatus=internalComponentErr; }
    }

    CFRelease(names);
    return status;

    END_SECAPI2("SecIdentityAddPreferenceItem")
}

OSStatus SecIdentityUpdatePreferenceItem(
			SecKeychainItemRef itemRef,
			SecIdentityRef identityRef)
{
    BEGIN_SECAPI

	if (!itemRef || !identityRef)
		MacOSError::throwMe(paramErr);
	SecPointer<Certificate> cert(Identity::required(identityRef)->certificate());
	Item prefItem = ItemImpl::required(itemRef);

	// account attribute (use label of certificate)
	char idUTF8[MAXPATHLEN];
	CFStringRef labelString = nil;
	cert->inferLabel(false, &labelString);
	if (!labelString || !CFStringGetCString(labelString, idUTF8, sizeof(idUTF8)-1, kCFStringEncodingUTF8))
		MacOSError::throwMe(errSecDataTooLarge);
	CssmData account(const_cast<void *>(reinterpret_cast<const void *>(idUTF8)), strlen(idUTF8));
	prefItem->setAttribute(Schema::attributeInfo(kSecAccountItemAttr), account);
	CFRelease(labelString);

	// generic attribute (store persistent certificate reference)
	CFDataRef pItemRef = nil;
	OSStatus status = SecKeychainItemCreatePersistentReference((SecKeychainItemRef)cert->handle(), &pItemRef);
	if (!pItemRef)
		status = errSecInvalidItemRef;
	if (status)
		MacOSError::throwMe(status);
	const UInt8 *dataPtr = CFDataGetBytePtr(pItemRef);
	CFIndex dataLen = CFDataGetLength(pItemRef);
	CssmData pref(const_cast<void *>(reinterpret_cast<const void *>(dataPtr)), dataLen);
	prefItem->setAttribute(Schema::attributeInfo(kSecGenericItemAttr), pref);
	CFRelease(pItemRef);

	prefItem->update();

    END_SECAPI2("SecIdentityUpdatePreferenceItem")
}

OSStatus SecIdentityCopyFromPreferenceItem(
			SecKeychainItemRef itemRef,
			SecIdentityRef *identityRef)
{
    BEGIN_SECAPI

	if (!itemRef || !identityRef)
		MacOSError::throwMe(paramErr);
	Item prefItem = ItemImpl::required(itemRef);

	// get persistent certificate reference
	SecKeychainAttribute itemAttrs[] = { { kSecGenericItemAttr, 0, NULL } };
	SecKeychainAttributeList itemAttrList = { sizeof(itemAttrs) / sizeof(itemAttrs[0]), itemAttrs };
	prefItem->getContent(NULL, &itemAttrList, NULL, NULL);

	// find certificate, given persistent reference data
	CFDataRef pItemRef = CFDataCreateWithBytesNoCopy(NULL, (const UInt8 *)itemAttrs[0].data, itemAttrs[0].length, kCFAllocatorNull);
	SecKeychainItemRef certItemRef = nil;
	OSStatus status = SecKeychainItemCopyFromPersistentReference(pItemRef, &certItemRef); //%%% need to make this a method of ItemImpl
	prefItem->freeContent(&itemAttrList, NULL);
	if (pItemRef)
		CFRelease(pItemRef);
	if (status)
		return status;

	// create identity reference, given certificate
	StorageManager::KeychainList keychains;
	globals().storageManager.optionalSearchList((CFTypeRef)NULL, keychains);
	Item certItem = ItemImpl::required(SecKeychainItemRef(certItemRef));
	SecPointer<Certificate> certificate(static_cast<Certificate *>(certItem.get()));
	SecPointer<Identity> identity(new Identity(keychains, certificate));
	if (certItemRef)
		CFRelease(certItemRef);

	Required(identityRef) = identity->handle();

    END_SECAPI2("SecIdentityCopyFromPreferenceItem")
}

/*
 * System Identity Support.
 */

/* plist domain (in /Library/Preferences) */
#define IDENTITY_DOMAIN		"com.apple.security.systemidentities"

/* 
 * Our plist is a dictionary whose entries have the following format:
 * key   = domain name as CFString
 * value = public key hash as CFData
 */

#define SYSTEM_KEYCHAIN_PATH	kSystemKeychainDir "/" kSystemKeychainName

/* 
 * All accesses to system identities and its associated plist are
 * protected by this lock.
 */
ModuleNexus<Mutex> systemIdentityLock;

OSStatus SecIdentityCopySystemIdentity(
   CFStringRef domain,          
   SecIdentityRef *idRef,
   CFStringRef *actualDomain) /* optional */
{
    BEGIN_SECAPI

	StLock<Mutex> _(systemIdentityLock());
	auto_ptr<Dictionary> identDict;
	
	/* get top-level dictionary - if not present, we're done */
	try {
		identDict.reset(new Dictionary(IDENTITY_DOMAIN, Dictionary::US_System));
	}
	catch(...) {
		MacOSError::throwMe(errSecNotAvailable);
	}
	
	/* see if there's an entry for specified domain */
	CFDataRef entryValue = identDict->getDataValue(domain);
	if(entryValue == NULL) {
		/* try for default entry if we're not already looking for default */
		if(!CFEqual(domain, kSecIdentityDomainDefault)) {
			entryValue = identDict->getDataValue(kSecIdentityDomainDefault);
		}
		if(entryValue == NULL) {
			/* no default identity */
			MacOSError::throwMe(errSecItemNotFound);
		}
		
		/* remember that we're not fetching the requested domain */
		domain = kSecIdentityDomainDefault;
	}
	
	/* open system keychain - error here is fatal */
	Keychain systemKc = globals().storageManager.make(SYSTEM_KEYCHAIN_PATH, false);
	CFRef<SecKeychainRef> systemKcRef(systemKc->handle());
	StorageManager::KeychainList keychains;
	globals().storageManager.optionalSearchList(systemKcRef, keychains);
	
	/* search for specified cert */
	SecKeychainAttributeList	attrList;
	SecKeychainAttribute		attr;
	attr.tag        = kSecPublicKeyHashItemAttr;
	attr.length     = CFDataGetLength(entryValue);
	attr.data       = (void *)CFDataGetBytePtr(entryValue);
	attrList.count  = 1;
	attrList.attr   = &attr;

	KCCursor cursor(keychains, kSecCertificateItemClass, &attrList);
	Item certItem;
	if(!cursor->next(certItem)) {
		MacOSError::throwMe(errSecItemNotFound);
	}
	
	/* found the cert; try matching with key to cook up identity */
	SecPointer<Certificate> certificate(static_cast<Certificate *>(certItem.get()));
	SecPointer<Identity> identity(new Identity(keychains, certificate));

	Required(idRef) = identity->handle();
	if(actualDomain) {
		*actualDomain = domain;
		CFRetain(*actualDomain);
	}

    END_SECAPI2("SecIdentityCopySystemIdentity")
}

OSStatus SecIdentitySetSystemIdentity(
   CFStringRef domain,     
   SecIdentityRef idRef)
{
    BEGIN_SECAPI

	StLock<Mutex> _(systemIdentityLock());
	if(geteuid() != 0) {
		MacOSError::throwMe(errSecAuthFailed);
	}
	auto_ptr<MutableDictionary> identDict;
	
	/* get top-level dictionary */
	try {
		identDict.reset(new MutableDictionary(IDENTITY_DOMAIN, Dictionary::US_System));
	}
	catch(...) {
		if(idRef == NULL) {
			/* nothing there, nothing to set - done */
			return noErr;
		}
		identDict.reset(new MutableDictionary());
	}
	
	if(idRef == NULL) {
		/* Just delete the possible entry for this domain */
		identDict->removeValue(domain);
	}
	else {
		/* obtain public key hash of identity's cert */
		SecPointer<Identity> identity(Identity::required(idRef));
		SecPointer<Certificate> cert = identity->certificate();
		const CssmData &pubKeyHash = cert->publicKeyHash();
		CFRef<CFDataRef> pubKeyHashData(CFDataCreate(NULL, pubKeyHash.Data, 
			pubKeyHash.Length));
		
		/* add/replace to dictionary */
		identDict->setValue(domain, pubKeyHashData);
	}
	
	/* flush to disk */
	if(!identDict->writePlistToPrefs(IDENTITY_DOMAIN, Dictionary::US_System)) {
		MacOSError::throwMe(ioErr);
	}

    END_SECAPI2("SecIdentitySetSystemIdentity")
}

const CFStringRef kSecIdentityDomainDefault = CFSTR("com.apple.systemdefault");
const CFStringRef kSecIdentityDomainKerberosKDC = CFSTR("com.apple.kerberos.kdc");

