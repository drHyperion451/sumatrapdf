/* Copyright 2015 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

namespace PdfEngine {

bool IsSupportedFile(const WCHAR* fileName, bool sniff = false);
BaseEngine* CreateFromFile(const WCHAR* fileName, PasswordUI* pwdUI = nullptr);
BaseEngine* CreateFromStream(IStream* stream, PasswordUI* pwdUI = nullptr);

} // namespace PdfEngine

namespace XpsEngine {

bool IsSupportedFile(const WCHAR* fileName, bool sniff = false);
BaseEngine* CreateFromFile(const WCHAR* fileName);
BaseEngine* CreateFromStream(IStream* stream);

} // namespace XpsEngine
