/*
 * Copyright (C) 2006-2007, 2010-2011, 2016 Apple Inc. All rights reserved.
 *           (C) 2007 Graham Dennis (graham.dennis@gmail.com)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer. 
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution. 
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ResourceLoader.h"

#include "ApplicationCacheHost.h"
#include "AuthenticationChallenge.h"
#include "DataURLDecoder.h"
#include "DiagnosticLoggingClient.h"
#include "DiagnosticLoggingKeys.h"
#include "DocumentLoader.h"
#include "Frame.h"
#include "FrameLoader.h"
#include "FrameLoaderClient.h"
#include "InspectorInstrumentation.h"
#include "LoaderStrategy.h"
#include "MainFrame.h"
#include "MixedContentChecker.h"
#include "Page.h"
#include "PlatformStrategies.h"
#include "ProgressTracker.h"
#include "ResourceError.h"
#include "ResourceHandle.h"
#include "SecurityOrigin.h"
#include "SharedBuffer.h"
#include <wtf/CompletionHandler.h>
#include <wtf/Ref.h>

#if ENABLE(CONTENT_EXTENSIONS)
#include "UserContentController.h"
#endif

#if USE(QUICK_LOOK)
#include "PreviewConverter.h"
#include "PreviewLoader.h"
#endif

namespace WebCore {

ResourceLoader::ResourceLoader(Frame& frame, ResourceLoaderOptions options)
    : m_frame { &frame }
    , m_documentLoader { frame.loader().activeDocumentLoader() }
    , m_defersLoading { options.defersLoadingPolicy == DefersLoadingPolicy::AllowDefersLoading && frame.page()->defersLoading() }
    , m_canAskClientForCredentials { options.clientCredentialPolicy == ClientCredentialPolicy::MayAskClientForCredentials }
    , m_options { options }
{
}

ResourceLoader::~ResourceLoader()
{
    ASSERT(m_reachedTerminalState);
}

void ResourceLoader::finishNetworkLoad()
{
    platformStrategies()->loaderStrategy()->remove(this);

    if (m_handle) {
        ASSERT(m_handle->client() == this);
        m_handle->clearClient();
        m_handle = nullptr;
    }
}

void ResourceLoader::releaseResources()
{
    ASSERT(!m_reachedTerminalState);
    
    // It's possible that when we release the handle, it will be
    // deallocated and release the last reference to this object.
    // We need to retain to avoid accessing the object after it
    // has been deallocated and also to avoid reentering this method.
    Ref<ResourceLoader> protectedThis(*this);

    m_frame = nullptr;
    m_documentLoader = nullptr;
    
    // We need to set reachedTerminalState to true before we release
    // the resources to prevent a double dealloc of WebView <rdar://problem/4372628>
    m_reachedTerminalState = true;

    finishNetworkLoad();

    m_identifier = 0;

    m_resourceData = nullptr;
    m_deferredRequest = ResourceRequest();
}

void ResourceLoader::init(ResourceRequest&& clientRequest, CompletionHandler<void(bool)>&& completionHandler)
{
    ASSERT(!m_handle);
    ASSERT(m_request.isNull());
    ASSERT(m_deferredRequest.isNull());
    ASSERT(!m_documentLoader->isSubstituteLoadPending(this));
    
    m_loadTiming.markStartTimeAndFetchStart();

#if PLATFORM(IOS)
    // If the documentLoader was detached while this ResourceLoader was waiting its turn
    // in ResourceLoadScheduler queue, don't continue.
    if (!m_documentLoader->frame()) {
        cancel();
        return completionHandler(false);
    }
#endif
    
    m_defersLoading = m_options.defersLoadingPolicy == DefersLoadingPolicy::AllowDefersLoading && m_frame->page()->defersLoading();
    m_canAskClientForCredentials = m_options.clientCredentialPolicy == ClientCredentialPolicy::MayAskClientForCredentials && !isMixedContent(clientRequest.url());

    if (m_options.securityCheck == DoSecurityCheck && !m_frame->document()->securityOrigin().canDisplay(clientRequest.url())) {
        FrameLoader::reportLocalLoadFailed(m_frame.get(), clientRequest.url().string());
        releaseResources();
        return completionHandler(false);
    }
    
    // https://bugs.webkit.org/show_bug.cgi?id=26391
    // The various plug-in implementations call directly to ResourceLoader::load() instead of piping requests
    // through FrameLoader. As a result, they miss the FrameLoader::addExtraFieldsToRequest() step which sets
    // up the 1st party for cookies URL. Until plug-in implementations can be reigned in to pipe through that
    // method, we need to make sure there is always a 1st party for cookies set.
    if (clientRequest.firstPartyForCookies().isNull()) {
        if (Document* document = m_frame->document())
            clientRequest.setFirstPartyForCookies(document->firstPartyForCookies());
    }

    willSendRequestInternal(WTFMove(clientRequest), ResourceResponse(), [this, protectedThis = makeRef(*this), completionHandler = WTFMove(completionHandler)](ResourceRequest&& request) mutable {

#if PLATFORM(IOS)
        // If this ResourceLoader was stopped as a result of willSendRequest, bail out.
        if (m_reachedTerminalState)
            return completionHandler(false);
#endif

        if (request.isNull()) {
            cancel();
            return completionHandler(false);
        }

        m_request = WTFMove(request);
        m_originalRequest = m_request;
        completionHandler(true);
    });
}

void ResourceLoader::deliverResponseAndData(const ResourceResponse& response, RefPtr<SharedBuffer>&& buffer)
{
    Ref<ResourceLoader> protectedThis(*this);

    didReceiveResponse(response);
    if (reachedTerminalState())
        return;

    if (buffer) {
        unsigned size = buffer->size();
        didReceiveBuffer(buffer.releaseNonNull(), size, DataPayloadWholeResource);
        if (reachedTerminalState())
            return;
    }

    NetworkLoadMetrics emptyMetrics;
    didFinishLoading(emptyMetrics);
}

void ResourceLoader::start()
{
    ASSERT(!m_handle);
    ASSERT(!m_request.isNull());
    ASSERT(m_deferredRequest.isNull());
    ASSERT(frameLoader());

#if ENABLE(WEB_ARCHIVE) || ENABLE(MHTML)
    if (m_documentLoader->scheduleArchiveLoad(*this, m_request))
        return;
#endif

    if (m_documentLoader->applicationCacheHost().maybeLoadResource(*this, m_request, m_request.url()))
        return;

    if (m_defersLoading) {
        m_deferredRequest = m_request;
        return;
    }

    if (m_reachedTerminalState)
        return;

    if (m_request.url().protocolIsData()) {
        loadDataURL();
        return;
    }

    m_handle = ResourceHandle::create(frameLoader()->networkingContext(), m_request, this, m_defersLoading, m_options.sniffContent == SniffContent, m_options.sniffContentEncoding == ContentEncodingSniffingPolicy::Sniff);
}

void ResourceLoader::setDefersLoading(bool defers)
{
    if (m_options.defersLoadingPolicy == DefersLoadingPolicy::DisallowDefersLoading)
        return;

    m_defersLoading = defers;
    if (m_handle)
        m_handle->setDefersLoading(defers);
    if (!defers && !m_deferredRequest.isNull()) {
        m_request = m_deferredRequest;
        m_deferredRequest = ResourceRequest();
        start();
    }

    platformStrategies()->loaderStrategy()->setDefersLoading(this, defers);
}

FrameLoader* ResourceLoader::frameLoader() const
{
    if (!m_frame)
        return nullptr;
    return &m_frame->loader();
}

void ResourceLoader::loadDataURL()
{
    auto url = m_request.url();
    ASSERT(url.protocolIsData());

    DataURLDecoder::ScheduleContext scheduleContext;
#if HAVE(RUNLOOP_TIMER)
    if (auto* scheduledPairs = m_frame->page()->scheduledRunLoopPairs())
        scheduleContext.scheduledPairs = *scheduledPairs;
#endif
    DataURLDecoder::decode(url, scheduleContext, [protectedThis = makeRef(*this), url](auto decodeResult) {
        if (protectedThis->reachedTerminalState())
            return;
        if (!decodeResult) {
            protectedThis->didFail(ResourceError(errorDomainWebKitInternal, 0, url, "Data URL decoding failed"));
            return;
        }
        if (protectedThis->wasCancelled())
            return;
        auto& result = decodeResult.value();
        auto dataSize = result.data ? result.data->size() : 0;

        ResourceResponse dataResponse { url, result.mimeType, static_cast<long long>(dataSize), result.charset };
        dataResponse.setHTTPStatusCode(200);
        dataResponse.setHTTPStatusText(ASCIILiteral("OK"));
        dataResponse.setHTTPHeaderField(HTTPHeaderName::ContentType, result.contentType);
        dataResponse.setSource(ResourceResponse::Source::Network);
        protectedThis->didReceiveResponse(dataResponse);

        if (!protectedThis->reachedTerminalState() && dataSize)
            protectedThis->didReceiveBuffer(result.data.releaseNonNull(), dataSize, DataPayloadWholeResource);

        if (!protectedThis->reachedTerminalState()) {
            NetworkLoadMetrics emptyMetrics;
            protectedThis->didFinishLoading(emptyMetrics);
        }
    });
}

void ResourceLoader::setDataBufferingPolicy(DataBufferingPolicy dataBufferingPolicy)
{
    m_options.dataBufferingPolicy = dataBufferingPolicy;

    // Reset any already buffered data
    if (dataBufferingPolicy == DoNotBufferData)
        m_resourceData = nullptr;
}

void ResourceLoader::willSwitchToSubstituteResource()
{
    ASSERT(!m_documentLoader->isSubstituteLoadPending(this));
    platformStrategies()->loaderStrategy()->remove(this);
    if (m_handle)
        m_handle->cancel();
}

void ResourceLoader::addDataOrBuffer(const char* data, unsigned length, SharedBuffer* buffer, DataPayloadType dataPayloadType)
{
    if (m_options.dataBufferingPolicy == DoNotBufferData)
        return;

    if (!m_resourceData || dataPayloadType == DataPayloadWholeResource) {
        if (buffer)
            m_resourceData = buffer;
        else
            m_resourceData = SharedBuffer::create(data, length);
        return;
    }
    
    if (buffer)
        m_resourceData->append(*buffer);
    else
        m_resourceData->append(data, length);
}

void ResourceLoader::clearResourceData()
{
    if (m_resourceData)
        m_resourceData->clear();
}

bool ResourceLoader::isSubresourceLoader()
{
    return false;
}

bool ResourceLoader::isMixedContent(const URL& url) const
{
    if (MixedContentChecker::isMixedContent(m_frame->document()->securityOrigin(), url))
        return true;
    Frame& topFrame = m_frame->tree().top();
    if (&topFrame != m_frame && MixedContentChecker::isMixedContent(topFrame.document()->securityOrigin(), url))
        return true;
    return false;
}

void ResourceLoader::willSendRequestInternal(ResourceRequest&& request, const ResourceResponse& redirectResponse, CompletionHandler<void(ResourceRequest&&)>&& completionHandler)
{
    // Protect this in this delegate method since the additional processing can do
    // anything including possibly derefing this; one example of this is Radar 3266216.
    Ref<ResourceLoader> protectedThis(*this);

    ASSERT(!m_reachedTerminalState);
#if ENABLE(CONTENT_EXTENSIONS)
    ASSERT(m_resourceType != ResourceType::Invalid);
#endif

    // We need a resource identifier for all requests, even if FrameLoader is never going to see it (such as with CORS preflight requests).
    bool createdResourceIdentifier = false;
    if (!m_identifier) {
        m_identifier = m_frame->page()->progress().createUniqueIdentifier();
        createdResourceIdentifier = true;
    }

#if ENABLE(CONTENT_EXTENSIONS)
    if (!redirectResponse.isNull() && frameLoader()) {
        Page* page = frameLoader()->frame().page();
        if (page && m_documentLoader) {
            auto blockedStatus = page->userContentProvider().processContentExtensionRulesForLoad(request.url(), m_resourceType, *m_documentLoader);
            applyBlockedStatusToRequest(blockedStatus, page, request);
            if (blockedStatus.blockedLoad) {
                didFail(blockedByContentBlockerError());
                completionHandler({ });
                return;
            }
        }
    }
#endif

    if (request.isNull()) {
        didFail(cannotShowURLError());
        completionHandler({ });
        return;
    }

    if (m_options.sendLoadCallbacks == SendCallbacks) {
        if (createdResourceIdentifier)
            frameLoader()->notifier().assignIdentifierToInitialRequest(m_identifier, documentLoader(), request);

#if PLATFORM(IOS)
        // If this ResourceLoader was stopped as a result of assignIdentifierToInitialRequest, bail out
        if (m_reachedTerminalState) {
            completionHandler(WTFMove(request));
            return;
        }
#endif

        frameLoader()->notifier().willSendRequest(this, request, redirectResponse);
    }
    else
        InspectorInstrumentation::willSendRequest(m_frame.get(), m_identifier, m_frame->loader().documentLoader(), request, redirectResponse);

#if USE(QUICK_LOOK)
    if (auto previewConverter = m_documentLoader->previewConverter())
        request = previewConverter->safeRequest(request);
#endif

    bool isRedirect = !redirectResponse.isNull();

    if (isMixedContent(m_request.url()) || (isRedirect && isMixedContent(request.url())))
        m_canAskClientForCredentials = false;

    if (isRedirect)
        platformStrategies()->loaderStrategy()->crossOriginRedirectReceived(this, request.url());

    m_request = request;

    if (isRedirect) {
        auto& redirectURL = request.url();
        if (!m_documentLoader->isCommitted())
            frameLoader()->client().dispatchDidReceiveServerRedirectForProvisionalLoad();

        if (redirectURL.protocolIsData()) {
            // Handle data URL decoding locally.
            finishNetworkLoad();
            loadDataURL();
        }
    }
    completionHandler(WTFMove(request));
}

void ResourceLoader::willSendRequest(ResourceRequest&& request, const ResourceResponse& redirectResponse, CompletionHandler<void(ResourceRequest&&)>&& completionHandler)
{
    willSendRequestInternal(WTFMove(request), redirectResponse, WTFMove(completionHandler));
}

void ResourceLoader::didSendData(unsigned long long, unsigned long long)
{
}

static void logResourceResponseSource(Frame* frame, ResourceResponse::Source source)
{
    if (!frame || !frame->page())
        return;

    String sourceKey;
    switch (source) {
    case ResourceResponse::Source::Network:
        sourceKey = DiagnosticLoggingKeys::networkKey();
        break;
    case ResourceResponse::Source::DiskCache:
        sourceKey = DiagnosticLoggingKeys::diskCacheKey();
        break;
    case ResourceResponse::Source::DiskCacheAfterValidation:
        sourceKey = DiagnosticLoggingKeys::diskCacheAfterValidationKey();
        break;
    case ResourceResponse::Source::ServiceWorker:
        sourceKey = DiagnosticLoggingKeys::serviceWorkerKey();
        break;
    case ResourceResponse::Source::MemoryCache:
    case ResourceResponse::Source::MemoryCacheAfterValidation:
    case ResourceResponse::Source::Unknown:
        return;
    }

    frame->page()->diagnosticLoggingClient().logDiagnosticMessage(DiagnosticLoggingKeys::resourceResponseSourceKey(), sourceKey, ShouldSample::Yes);
}

void ResourceLoader::didReceiveResponse(const ResourceResponse& r)
{
    ASSERT(!m_reachedTerminalState);

    // Protect this in this delegate method since the additional processing can do
    // anything including possibly derefing this; one example of this is Radar 3266216.
    Ref<ResourceLoader> protectedThis(*this);

    logResourceResponseSource(m_frame.get(), r.source());

    m_response = r;

    if (FormData* data = m_request.httpBody())
        data->removeGeneratedFilesIfNeeded();

    if (m_options.sendLoadCallbacks == SendCallbacks)
        frameLoader()->notifier().didReceiveResponse(this, m_response);
}

void ResourceLoader::didReceiveData(const char* data, unsigned length, long long encodedDataLength, DataPayloadType dataPayloadType)
{
    // The following assertions are not quite valid here, since a subclass
    // might override didReceiveData in a way that invalidates them. This
    // happens with the steps listed in 3266216
    // ASSERT(con == connection);
    // ASSERT(!m_reachedTerminalState);

    didReceiveDataOrBuffer(data, length, nullptr, encodedDataLength, dataPayloadType);
}

void ResourceLoader::didReceiveBuffer(Ref<SharedBuffer>&& buffer, long long encodedDataLength, DataPayloadType dataPayloadType)
{
    didReceiveDataOrBuffer(nullptr, 0, WTFMove(buffer), encodedDataLength, dataPayloadType);
}

void ResourceLoader::didReceiveDataOrBuffer(const char* data, unsigned length, RefPtr<SharedBuffer>&& buffer, long long encodedDataLength, DataPayloadType dataPayloadType)
{
    // This method should only get data+length *OR* a SharedBuffer.
    ASSERT(!buffer || (!data && !length));

    // Protect this in this delegate method since the additional processing can do
    // anything including possibly derefing this; one example of this is Radar 3266216.
    Ref<ResourceLoader> protectedThis(*this);

    addDataOrBuffer(data, length, buffer.get(), dataPayloadType);

    // FIXME: If we get a resource with more than 2B bytes, this code won't do the right thing.
    // However, with today's computers and networking speeds, this won't happen in practice.
    // Could be an issue with a giant local file.
    if (m_options.sendLoadCallbacks == SendCallbacks && m_frame)
        frameLoader()->notifier().didReceiveData(this, buffer ? buffer->data() : data, buffer ? buffer->size() : length, static_cast<int>(encodedDataLength));
}

void ResourceLoader::didFinishLoading(const NetworkLoadMetrics& networkLoadMetrics)
{
    didFinishLoadingOnePart(networkLoadMetrics);

    // If the load has been cancelled by a delegate in response to didFinishLoad(), do not release
    // the resources a second time, they have been released by cancel.
    if (wasCancelled())
        return;
    releaseResources();
}

void ResourceLoader::didFinishLoadingOnePart(const NetworkLoadMetrics& networkLoadMetrics)
{
    // If load has been cancelled after finishing (which could happen with a
    // JavaScript that changes the window location), do nothing.
    if (wasCancelled())
        return;
    ASSERT(!m_reachedTerminalState);

    if (m_notifiedLoadComplete)
        return;
    m_notifiedLoadComplete = true;
    if (m_options.sendLoadCallbacks == SendCallbacks)
        frameLoader()->notifier().didFinishLoad(this, networkLoadMetrics);
}

void ResourceLoader::didFail(const ResourceError& error)
{
    if (wasCancelled())
        return;
    ASSERT(!m_reachedTerminalState);

    // Protect this in this delegate method since the additional processing can do
    // anything including possibly derefing this; one example of this is Radar 3266216.
    Ref<ResourceLoader> protectedThis(*this);

    cleanupForError(error);
    releaseResources();
}

void ResourceLoader::cleanupForError(const ResourceError& error)
{
    if (FormData* data = m_request.httpBody())
        data->removeGeneratedFilesIfNeeded();

    if (m_notifiedLoadComplete)
        return;
    m_notifiedLoadComplete = true;
    if (m_options.sendLoadCallbacks == SendCallbacks && m_identifier)
        frameLoader()->notifier().didFailToLoad(this, error);
}

void ResourceLoader::cancel()
{
    cancel(ResourceError());
}

void ResourceLoader::cancel(const ResourceError& error)
{
    // If the load has already completed - succeeded, failed, or previously cancelled - do nothing.
    if (m_reachedTerminalState)
        return;
       
    ResourceError nonNullError = error.isNull() ? cancelledError() : error;
    
    // willCancel() and didFailToLoad() both call out to clients that might do 
    // something causing the last reference to this object to go away.
    Ref<ResourceLoader> protectedThis(*this);
    
    // If we re-enter cancel() from inside willCancel(), we want to pick up from where we left 
    // off without re-running willCancel()
    if (m_cancellationStatus == NotCancelled) {
        m_cancellationStatus = CalledWillCancel;
        
        willCancel(nonNullError);
    }

    // If we re-enter cancel() from inside didFailToLoad(), we want to pick up from where we 
    // left off without redoing any of this work.
    if (m_cancellationStatus == CalledWillCancel) {
        m_cancellationStatus = Cancelled;

        if (m_handle)
            m_handle->clearAuthentication();

        m_documentLoader->cancelPendingSubstituteLoad(this);
        if (m_handle) {
            m_handle->cancel();
            m_handle = nullptr;
        }
        cleanupForError(nonNullError);
    }

    // If cancel() completed from within the call to willCancel() or didFailToLoad(),
    // we don't want to redo didCancel() or releasesResources().
    if (m_reachedTerminalState)
        return;

    didCancel(nonNullError);

    if (m_cancellationStatus == FinishedCancel)
        return;
    m_cancellationStatus = FinishedCancel;

    releaseResources();
}

ResourceError ResourceLoader::cancelledError()
{
    return frameLoader()->cancelledError(m_request);
}

ResourceError ResourceLoader::blockedError()
{
    return frameLoader()->client().blockedError(m_request);
}

ResourceError ResourceLoader::blockedByContentBlockerError()
{
    return frameLoader()->client().blockedByContentBlockerError(m_request);
}

ResourceError ResourceLoader::cannotShowURLError()
{
    return frameLoader()->client().cannotShowURLError(m_request);
}

void ResourceLoader::willSendRequestAsync(ResourceHandle* handle, ResourceRequest&& request, ResourceResponse&& redirectResponse, CompletionHandler<void(ResourceRequest&&)>&& completionHandler)
{
    RefPtr<ResourceHandle> protectedHandle(handle);
    if (documentLoader()->applicationCacheHost().maybeLoadFallbackForRedirect(this, request, redirectResponse)) {
        completionHandler(WTFMove(request));
        return;
    }
    willSendRequestInternal(WTFMove(request), redirectResponse, WTFMove(completionHandler));
}

void ResourceLoader::didSendData(ResourceHandle*, unsigned long long bytesSent, unsigned long long totalBytesToBeSent)
{
    didSendData(bytesSent, totalBytesToBeSent);
}

void ResourceLoader::didReceiveResponseAsync(ResourceHandle* handle, ResourceResponse&& response)
{
    if (documentLoader()->applicationCacheHost().maybeLoadFallbackForResponse(this, response)) {
        handle->continueDidReceiveResponse();
        return;
    }
    didReceiveResponse(response);
    handle->continueDidReceiveResponse();
}

void ResourceLoader::didReceiveData(ResourceHandle*, const char* data, unsigned length, int encodedDataLength)
{
    didReceiveData(data, length, encodedDataLength, DataPayloadBytes);
}

void ResourceLoader::didReceiveBuffer(ResourceHandle*, Ref<SharedBuffer>&& buffer, int encodedDataLength)
{
    didReceiveBuffer(WTFMove(buffer), encodedDataLength, DataPayloadBytes);
}

void ResourceLoader::didFinishLoading(ResourceHandle*)
{
    NetworkLoadMetrics emptyMetrics;
    didFinishLoading(emptyMetrics);
}

void ResourceLoader::didFail(ResourceHandle*, const ResourceError& error)
{
    if (documentLoader()->applicationCacheHost().maybeLoadFallbackForError(this, error))
        return;
    didFail(error);
}

void ResourceLoader::wasBlocked(ResourceHandle*)
{
    didFail(blockedError());
}

void ResourceLoader::cannotShowURL(ResourceHandle*)
{
    didFail(cannotShowURLError());
}

bool ResourceLoader::shouldUseCredentialStorage()
{
    if (m_options.storedCredentialsPolicy == StoredCredentialsPolicy::DoNotUse)
        return false;

    Ref<ResourceLoader> protectedThis(*this);
    return frameLoader()->client().shouldUseCredentialStorage(documentLoader(), identifier());
}

bool ResourceLoader::isAllowedToAskUserForCredentials() const
{
    if (!m_canAskClientForCredentials)
        return false;
    return m_options.credentials == FetchOptions::Credentials::Include || (m_options.credentials == FetchOptions::Credentials::SameOrigin && m_frame->document()->securityOrigin().canRequest(originalRequest().url()));
}

void ResourceLoader::didReceiveAuthenticationChallenge(const AuthenticationChallenge& challenge)
{
    ASSERT(m_handle->hasAuthenticationChallenge());

    // Protect this in this delegate method since the additional processing can do
    // anything including possibly derefing this; one example of this is Radar 3266216.
    Ref<ResourceLoader> protectedThis(*this);

    if (m_options.storedCredentialsPolicy == StoredCredentialsPolicy::Use) {
        if (isAllowedToAskUserForCredentials()) {
            frameLoader()->notifier().didReceiveAuthenticationChallenge(this, challenge);
            return;
        }
    }
    challenge.authenticationClient()->receivedRequestToContinueWithoutCredential(challenge);
    ASSERT(!m_handle || !m_handle->hasAuthenticationChallenge());
}

#if USE(PROTECTION_SPACE_AUTH_CALLBACK)
void ResourceLoader::canAuthenticateAgainstProtectionSpaceAsync(ResourceHandle* handle, const ProtectionSpace& protectionSpace)
{
    handle->continueCanAuthenticateAgainstProtectionSpace(canAuthenticateAgainstProtectionSpace(protectionSpace));
}

bool ResourceLoader::canAuthenticateAgainstProtectionSpace(const ProtectionSpace& protectionSpace)
{
    Ref<ResourceLoader> protectedThis(*this);
    return frameLoader()->client().canAuthenticateAgainstProtectionSpace(documentLoader(), identifier(), protectionSpace);
}

#endif
    
#if PLATFORM(IOS)

RetainPtr<CFDictionaryRef> ResourceLoader::connectionProperties(ResourceHandle*)
{
    return frameLoader()->connectionProperties(this);
}

#endif

void ResourceLoader::receivedCancellation(const AuthenticationChallenge&)
{
    cancel();
}

#if PLATFORM(COCOA)

void ResourceLoader::schedule(SchedulePair& pair)
{
    if (m_handle)
        m_handle->schedule(pair);
}

void ResourceLoader::unschedule(SchedulePair& pair)
{
    if (m_handle)
        m_handle->unschedule(pair);
}

#endif

#if USE(QUICK_LOOK)
bool ResourceLoader::isQuickLookResource() const
{
    return !!m_previewLoader;
}
#endif

bool ResourceLoader::isAlwaysOnLoggingAllowed() const
{
    return frameLoader() && frameLoader()->isAlwaysOnLoggingAllowed();
}

void ResourceLoader::didRetrieveDerivedDataFromCache(const String&, SharedBuffer&)
{
}

}
