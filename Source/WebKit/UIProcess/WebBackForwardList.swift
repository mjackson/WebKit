// Copyright (C) 2025-2026 Apple Inc. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
// BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.

#if compiler(>=6.2)

#if ENABLE_BACK_FORWARD_LIST_SWIFT

internal import WebCore_Private
internal import WebKit_Internal
internal import wtf

final class WebBackForwardList {
    private static let defaultCapacity = 100

    var page: WebKit.WeakPtrWebPageProxy
    // Optional just because of an initialization order issue.
    // Always occupied after initialization finished.
    var messageForwarder: RefWebBackForwardListMessageForwarder?

    var entries: [WebKit.WebBackForwardListItem] = []
    var currentIndex: Array.Index?

    private enum Direction {
        case backward
        case forward
    }

    init(page: WebKit.WeakPtrWebPageProxy) {
        self.page = page
        self.messageForwarder = WebKit.WebBackForwardListMessageForwarder.create(target: self)
    }

    func getMessageReceiver() -> RefWebBackForwardListMessageForwarder {
        // Guaranteed to be Some after construction
        // swift-format-ignore: NeverForceUnwrap
        self.messageForwarder!
    }

    func itemForID(identifier: WebCore.BackForwardItemIdentifier) -> WebKit.WebBackForwardListItem? {
    }

    func pageClosed() {
    }

    func addItem(newItem: WebKit.WebBackForwardListItem) {
    }

    func goToItem(item: WebKit.WebBackForwardListItem) {
    }

    func currentItem() -> WebKit.WebBackForwardListItem? {
    }

    func backItem() -> WebKit.WebBackForwardListItem? {
    }

    func forwardItem() -> WebKit.WebBackForwardListItem? {
    }

    func itemAtIndex(index: Array.Index) -> WebKit.WebBackForwardListItem? {
    }

    func backListCount() -> Array.Index {
    }

    func forwardListCount() -> Array.Index {
    }

    func backListAsAPIArrayWithLimit(limit: UInt) -> API.RefAPIArray {
    }

    func forwardListAsAPIArrayWithLimit(limit: UInt) -> API.RefAPIArray {
    }

    func removeAllItems() {
    }

    func clear() {
    }

    func backForwardListState(filter: WebBackForwardListItemFilter) -> WebKit.BackForwardListState {
    }

    func restoreFromState(backForwardListState: WebKit.BackForwardListState) {
    }

    func setItemsAsRestoredFromSession() {
    }

    func setItemsAsRestoredFromSessionIf(functor: WebBackForwardListItemFilter) {
    }

    func didRemoveItem(item: WebKit.WebBackForwardListItem) {
    }

    func goBackItemSkippingItemsWithoutUserGesture() -> WebKit.RefPtrWebBackForwardListItem {
    }

    func goForwardItemSkippingItemsWithoutUserGesture() -> WebKit.RefPtrWebBackForwardListItem {
    }

    func loggingString() -> Swift.String {
    }

    func setBackForwardItemIdentifier(frameState: WebKit.FrameState, itemID: WebCore.BackForwardItemIdentifier) {
    }

    func completeFrameStateForNavigation(navigatedFrameState: WebKit.FrameState) -> WebKit.FrameState {
    }

    func backForwardAddItemShared(
        connection: IPC.Connection,
        navigatedFrameState: WebKit.RefFrameState,
        loadedWebArchive: WebKit.LoadedWebArchive
    ) {
    }

    // IPCs from here on

    func backForwardAddItem(connection: IPC.Connection, navigatedFrameState: WebKit.RefFrameState) {
    }

    func backForwardSetChildItem(frameItemID: WebCore.BackForwardFrameItemIdentifier, frameState: WebKit.RefFrameState) {
    }

    func backForwardClearChildren(itemID: WebCore.BackForwardItemIdentifier, frameItemID: WebCore.BackForwardFrameItemIdentifier) {
    }

    func backForwardUpdateItem(connection: IPC.Connection, frameState: WebKit.RefFrameState) {
    }

    func backForwardGoToItem(
        itemID: WebCore.BackForwardItemIdentifier,
        completionHandler: CompletionHandlers.WebBackForwardList.BackForwardGoToItemCompletionHandler
    ) {
    }

    func backForwardListContainsItem(
        itemID: WebCore.BackForwardItemIdentifier,
        completionHandler: CompletionHandlers.WebBackForwardList.BackForwardListContainsItemCompletionHandler
    ) {
    }

    func backForwardGoToItemShared(
        itemID: WebCore.BackForwardItemIdentifier,
        completionHandler: CompletionHandlers.WebBackForwardList.BackForwardGoToItemCompletionHandler
    ) {
    }

    func frameStateForItem(item: WebKit.WebBackForwardListItem, frameID: WebCore.FrameIdentifier) -> WebKit.FrameState {
    }

    func backForwardAllItems(
        frameID: WebCore.FrameIdentifier,
        completionHandler: CompletionHandlers.WebBackForwardList.BackForwardAllItemsCompletionHandler
    ) {
    }

    func backForwardItemAtIndex(
        index: Int32,
        frameID: WebCore.FrameIdentifier,
        completionHandler: CompletionHandlers.WebBackForwardList.BackForwardItemAtIndexCompletionHandler
    ) {
    }

    func backForwardListCounts(completionHandler: CompletionHandlers.WebBackForwardList.BackForwardListCountsCompletionHandler) {
    }
}

#endif // ENABLE_BACK_FORWARD_LIST_SWIFT

#endif // compiler(>=6.2)
