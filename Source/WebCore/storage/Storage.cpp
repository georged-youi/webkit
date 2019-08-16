/*
 * Copyright (C) 2008 Apple Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "Storage.h"

#include "Document.h"
#include "ExceptionCode.h"
#include "Frame.h"
#include "Page.h"
#include "SchemeRegistry.h"
#include "SecurityOrigin.h"
#include "StorageArea.h"
#include "StorageType.h"
#include <wtf/text/WTFString.h>

namespace WebCore {

Ref<Storage> Storage::create(Frame* frame, RefPtr<StorageArea>&& storageArea)
{
    return adoptRef(*new Storage(frame, WTFMove(storageArea)));
}

Storage::Storage(Frame* frame, RefPtr<StorageArea>&& storageArea)
    : DOMWindowProperty(frame)
    , m_storageArea(WTFMove(storageArea))
{
    ASSERT(m_frame);
    ASSERT(m_storageArea);

    m_storageArea->incrementAccessCount();
}

Storage::~Storage()
{
    m_storageArea->decrementAccessCount();
}

ExceptionOr<unsigned> Storage::length() const
{
    if (!m_storageArea->canAccessStorage(m_frame))
        return Exception { SECURITY_ERR };

    return m_storageArea->length();
}

ExceptionOr<String> Storage::key(unsigned index) const
{
    if (!m_storageArea->canAccessStorage(m_frame))
        return Exception { SECURITY_ERR };

    return m_storageArea->key(index);
}

ExceptionOr<String> Storage::getItem(const String& key) const
{
    if (!m_storageArea->canAccessStorage(m_frame))
        return Exception { SECURITY_ERR };

    return m_storageArea->item(key);
}

ExceptionOr<void> Storage::setItem(const String& key, const String& value)
{
    if (!m_storageArea->canAccessStorage(m_frame))
        return Exception { SECURITY_ERR };

    bool quotaException = false;
    m_storageArea->setItem(m_frame, key, value, quotaException);
    if (quotaException)
        return Exception { QUOTA_EXCEEDED_ERR };
    return { };
}

ExceptionOr<void> Storage::removeItem(const String& key)
{
    if (!m_storageArea->canAccessStorage(m_frame))
        return Exception { SECURITY_ERR };

    m_storageArea->removeItem(m_frame, key);
    return { };
}

ExceptionOr<void> Storage::clear()
{
    if (!m_storageArea->canAccessStorage(m_frame))
        return Exception { SECURITY_ERR };

    m_storageArea->clear(m_frame);
    return { };
}

ExceptionOr<bool> Storage::contains(const String& key) const
{
    if (!m_storageArea->canAccessStorage(m_frame))
        return Exception { SECURITY_ERR };

    return m_storageArea->contains(key);
}

bool Storage::isSupportedPropertyName(const String& propertyName) const
{
    if (!m_storageArea->canAccessStorage(m_frame))
        return false;

    return m_storageArea->contains(propertyName);
}

} // namespace WebCore
