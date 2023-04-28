echo "Copy CEF binaries..."
xcopy /SY "Cef\Bin\" "%~2"

echo "Copy CEF resources..."
xcopy /SY "Cef\Resources" "%~2"

if "%~1"=="Release" (
	echo "Build Web Assets..."
	call "%~dp0build-web.bat"
)

echo "Copy web resources..."
xcopy /SY "Web\dist" "%~2Web\"

echo "Copy presets..."
xcopy /SY "Web\presets" "%~2Presets\"

if "%4"=="uiAccessOn" (
	echo "Sign executable..."
	SignTool sign /v /s PrivateCertStore /n "Test Certificate - For Internal Use Only" /t http://timestamp.comodoca.com/authenticode /fd sha1 "%~2%~3"
)