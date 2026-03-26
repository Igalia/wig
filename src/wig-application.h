/*
 * Copyright (c) 2026 Igalia S.L.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _WIG_APPLICATION_H_
#define _WIG_APPLICATION_H_

#include <adwaita.h>
#include <gtk/gtk.h>
#include <wpe/webkit.h>

G_BEGIN_DECLS

#define WIG_TYPE_APPLICATION (wig_application_get_type())
G_DECLARE_FINAL_TYPE(WigApplication, wig_application, WIG, APPLICATION, AdwApplication)

WigApplication       *wig_application_new                 (void);
WigApplication       *wig_application_get                 (void);
WPEDisplay           *wig_application_get_display         (WigApplication* app);
WebKitNetworkSession *wig_application_get_network_session (WigApplication* app);
WebKitWebContext     *wig_application_get_web_context     (WigApplication* app);
WebKitSettings       *wig_application_get_web_settings    (WigApplication* app);

G_END_DECLS

#endif /* _WIG_APPLICATION_H_ */
