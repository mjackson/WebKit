/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(DBLCLICK_EVENT_REGIONS)

#import "Helpers/PlatformUtilities.h"
#import "Helpers/Test.h"
#import "Helpers/cocoa/TestWKWebView.h"
#import <WebKit/WKWebViewPrivateForTesting.h>
#import <wtf/text/ASCIILiteral.h>

namespace TestWebKitAPI {

inline constexpr auto iframeContentForCrossOriginDblclickWindowListener = R"(
    <!DOCTYPE html>
    <html>
    <body style='margin: 0; padding: 0;'>
        <script>
            window.addEventListener('dblclick', function(e) {
                parent.postMessage({
                    type: 'dblclick',
                    clientX: e.clientX,
                    clientY: e.clientY
                }, '*');
            });
            requestAnimationFrame(() => parent.postMessage({ type: 'iframeReady' }, '*'));
        </script>
    </body>
    </html>
)"_s;

inline constexpr auto iframeContentForCrossOriginDblclickDocumentListener = R"(
    <!DOCTYPE html>
    <html>
    <body style='margin: 0; padding: 0;'>
        <script>
            document.addEventListener('dblclick', function(e) {
                parent.postMessage({
                    type: 'dblclick',
                    clientX: e.clientX,
                    clientY: e.clientY
                }, '*');
            });
            requestAnimationFrame(() => parent.postMessage({ type: 'iframeReady' }, '*'));
        </script>
    </body>
    </html>
)"_s;

inline constexpr auto mainHTMLForCrossOriginDblclick = R"(
    <!DOCTYPE html>
    <html>
    <body style='margin: 0; padding: 0;'>
        <iframe id='frame' src='https://webkit.org/iframe' style='width: 100px; height: 100px; position: absolute; border: none;'></iframe>
        <script>
            window.iframeReady = new Promise(resolve => {
                window.addEventListener('message', function(e) {
                    if (e.data.type === 'iframeReady')
                        resolve();
                });
            });
            window.dblclickReceived = new Promise(resolve => {
                window.addEventListener('message', function(e) {
                    if (e.data.type === 'dblclick') {
                        window.clientX = e.data.clientX;
                        window.clientY = e.data.clientY;
                        resolve();
                    }
                });
            });
        </script>
    </body>
    </html>
)"_s;

inline void testDblclickInCrossOriginIFrame(TestWKWebView *webView, CGFloat tapX, CGFloat tapY, NSString *expectedX, NSString *expectedY, NSString *jsTransform = nil)
{
    [webView objectByCallingAsyncFunction:@"return await window.iframeReady;" withArguments:@{}];
    [webView waitForNextPresentationUpdate];

    if (jsTransform) {
        __block bool done = false;
        [webView evaluateJavaScript:jsTransform completionHandler:^(id, NSError *) {
            done = true;
        }];
        Util::run(&done);
        [webView waitForNextPresentationUpdate];
    }

    [webView _simulateDoubleClickAtLocation:CGPointMake(tapX, tapY)];
    [webView objectByCallingAsyncFunction:@"return await window.dblclickReceived;" withArguments:@{}];

    EXPECT_WK_STREQ(expectedX, [webView stringByEvaluatingJavaScript:@"window.clientX"]);
    EXPECT_WK_STREQ(expectedY, [webView stringByEvaluatingJavaScript:@"window.clientY"]);
}

} // namespace TestWebKitAPI

#endif // ENABLE(DBLCLICK_EVENT_REGIONS)
