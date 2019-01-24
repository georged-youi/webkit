/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebSWClientConnection.h"

#if ENABLE(SERVICE_WORKER)

#include "DataReference.h"
#include "FormDataReference.h"
#include "Logging.h"
#include "ServiceWorkerClientFetch.h"
#include "StorageToWebProcessConnectionMessages.h"
#include "WebCoreArgumentCoders.h"
#include "WebSWOriginTable.h"
#include "WebSWServerConnectionMessages.h"
#include <WebCore/Document.h>
#include <WebCore/SerializedScriptValue.h>
#include <WebCore/ServiceWorkerClientData.h>
#include <WebCore/ServiceWorkerFetchResult.h>
#include <WebCore/ServiceWorkerJobData.h>
#include <WebCore/ServiceWorkerRegistrationData.h>

using namespace PAL;
using namespace WebCore;

namespace WebKit {

WebSWClientConnection::WebSWClientConnection(IPC::Connection& connection, SessionID sessionID)
    : m_sessionID(sessionID)
    , m_connection(connection)
    , m_swOriginTable(makeUniqueRef<WebSWOriginTable>())
{
    bool result = sendSync(Messages::StorageToWebProcessConnection::EstablishSWServerConnection(sessionID), Messages::StorageToWebProcessConnection::EstablishSWServerConnection::Reply(m_identifier));

    ASSERT_UNUSED(result, result);
}

WebSWClientConnection::~WebSWClientConnection()
{
}

void WebSWClientConnection::scheduleJobInServer(const ServiceWorkerJobData& jobData)
{
    send(Messages::WebSWServerConnection::ScheduleJobInServer(jobData));
}

void WebSWClientConnection::finishFetchingScriptInServer(const ServiceWorkerFetchResult& result)
{
    send(Messages::WebSWServerConnection::FinishFetchingScriptInServer(result));
}

void WebSWClientConnection::addServiceWorkerRegistrationInServer(const ServiceWorkerRegistrationKey& key, ServiceWorkerRegistrationIdentifier identifier)
{
    send(Messages::WebSWServerConnection::AddServiceWorkerRegistrationInServer(key, identifier));
}

void WebSWClientConnection::removeServiceWorkerRegistrationInServer(const ServiceWorkerRegistrationKey& key, ServiceWorkerRegistrationIdentifier identifier)
{
    send(Messages::WebSWServerConnection::RemoveServiceWorkerRegistrationInServer(key, identifier));
}

void WebSWClientConnection::postMessageToServiceWorkerGlobalScope(ServiceWorkerIdentifier destinationIdentifier, Ref<SerializedScriptValue>&& scriptValue, uint64_t sourceContextIdentifier, ServiceWorkerClientData&& source)
{
    send(Messages::WebSWServerConnection::PostMessageToServiceWorkerGlobalScope(destinationIdentifier, IPC::DataReference { scriptValue->data() }, sourceContextIdentifier, WTFMove(source)));
}

void WebSWClientConnection::serviceWorkerStartedControllingClient(ServiceWorkerIdentifier serviceWorkerIdentifier, uint64_t scriptExecutionContextIdentifier)
{
    send(Messages::WebSWServerConnection::ServiceWorkerStartedControllingClient(serviceWorkerIdentifier, scriptExecutionContextIdentifier));
}

void WebSWClientConnection::serviceWorkerStoppedControllingClient(ServiceWorkerIdentifier serviceWorkerIdentifier, uint64_t scriptExecutionContextIdentifier)
{
    send(Messages::WebSWServerConnection::ServiceWorkerStoppedControllingClient(serviceWorkerIdentifier, scriptExecutionContextIdentifier));
}

void WebSWClientConnection::didResolveRegistrationPromise(const ServiceWorkerRegistrationKey& key)
{
    send(Messages::WebSWServerConnection::DidResolveRegistrationPromise(key));
}

bool WebSWClientConnection::mayHaveServiceWorkerRegisteredForOrigin(const SecurityOrigin& origin) const
{
    if (!m_swOriginTable->isInitialized())
        return true;

    return m_swOriginTable->contains(origin);
}

void WebSWClientConnection::setSWOriginTableSharedMemory(const SharedMemory::Handle& handle)
{
    m_swOriginTable->setSharedMemory(handle);
}

void WebSWClientConnection::initializeSWOriginTableAsEmpty()
{
    m_swOriginTable->initializeAsEmpty();
}

void WebSWClientConnection::didMatchRegistration(uint64_t matchingRequest, std::optional<ServiceWorkerRegistrationData>&& result)
{
    if (auto completionHandler = m_ongoingMatchRegistrationTasks.take(matchingRequest))
        completionHandler(WTFMove(result));
}

void WebSWClientConnection::didGetRegistrations(uint64_t matchingRequest, Vector<ServiceWorkerRegistrationData>&& registrations)
{
    if (auto completionHandler = m_ongoingGetRegistrationsTasks.take(matchingRequest))
        completionHandler(WTFMove(registrations));
}

void WebSWClientConnection::matchRegistration(const SecurityOrigin& topOrigin, const URL& clientURL, RegistrationCallback&& callback)
{
    if (!mayHaveServiceWorkerRegisteredForOrigin(topOrigin)) {
        callback(std::nullopt);
        return;
    }

    uint64_t requestIdentifier = ++m_previousMatchRegistrationTaskIdentifier;
    m_ongoingMatchRegistrationTasks.add(requestIdentifier, WTFMove(callback));
    send(Messages::WebSWServerConnection::MatchRegistration(requestIdentifier, SecurityOriginData::fromSecurityOrigin(topOrigin), clientURL));
}

void WebSWClientConnection::getRegistrations(const SecurityOrigin& topOrigin, const URL& clientURL, GetRegistrationsCallback&& callback)
{
    if (!mayHaveServiceWorkerRegisteredForOrigin(topOrigin)) {
        callback({ });
        return;
    }

    uint64_t requestIdentifier = ++m_previousGetRegistrationsTaskIdentifier;
    m_ongoingGetRegistrationsTasks.add(requestIdentifier, WTFMove(callback));
    send(Messages::WebSWServerConnection::GetRegistrations(requestIdentifier, SecurityOriginData::fromSecurityOrigin(topOrigin), clientURL));
}

Ref<ServiceWorkerClientFetch> WebSWClientConnection::startFetch(WebServiceWorkerProvider& provider, Ref<WebCore::ResourceLoader>&& loader, uint64_t identifier, ServiceWorkerClientFetch::Callback&& callback)
{
    ASSERT(loader->options().serviceWorkersMode != ServiceWorkersMode::None && loader->options().serviceWorkerIdentifier);

    send(Messages::WebSWServerConnection::StartFetch { identifier, loader->options().serviceWorkerIdentifier, loader->originalRequest(), loader->options(), IPC::FormDataReference { loader->originalRequest().httpBody() } });
    return ServiceWorkerClientFetch::create(provider, WTFMove(loader), identifier, m_connection.get(), WTFMove(callback));
}

void WebSWClientConnection::postMessageToServiceWorkerClient(uint64_t destinationScriptExecutionContextIdentifier, const IPC::DataReference& message, ServiceWorkerData&& source, const String& sourceOrigin)
{
    SWClientConnection::postMessageToServiceWorkerClient(destinationScriptExecutionContextIdentifier, SerializedScriptValue::adopt(message.vector()), WTFMove(source), sourceOrigin);
}

} // namespace WebKit

#endif // ENABLE(SERVICE_WORKER)
