#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <future>
#include <sstream>
#include <iostream>

#pragma comment(lib, "winhttp.lib")

namespace dnd {

struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::vector<uint8_t> rawData;
    bool success = false;
    std::string errorMessage;
};

class WinHttpClient {
public:
    static HttpResponse Request(const std::string& method, const std::string& url, const std::string& body = "", const std::string& contentType = "application/json", const std::vector<uint8_t>& binaryBody = {}, int timeoutSeconds = 60) {
        HttpResponse resp;
        
        URL_COMPONENTS urlComp = {0};
        urlComp.dwStructSize = sizeof(urlComp);
        wchar_t hostName[256] = {0};
        wchar_t urlPath[1024] = {0};
        urlComp.lpszHostName = hostName;
        urlComp.dwHostNameLength = 256;
        urlComp.lpszUrlPath = urlPath;
        urlComp.dwUrlPathLength = 1024;

        std::wstring wUrl(url.begin(), url.end());
        if (!WinHttpCrackUrl(wUrl.c_str(), (DWORD)wUrl.length(), 0, &urlComp)) {
            resp.errorMessage = "Failed to parse URL";
            return resp;
        }

        HINTERNET hSession = WinHttpOpen(L"DndMapGenerator/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            resp.errorMessage = "Failed to open WinHTTP session";
            return resp;
        }

        WinHttpSetTimeouts(hSession, timeoutSeconds * 1000, timeoutSeconds * 1000, timeoutSeconds * 1000, timeoutSeconds * 1000);

        HINTERNET hConnect = WinHttpConnect(hSession, urlComp.lpszHostName, urlComp.nPort, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            resp.errorMessage = "Failed to connect to host";
            return resp;
        }

        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        std::wstring wMethod(method.begin(), method.end());
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, wMethod.c_str(), urlComp.lpszUrlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            resp.errorMessage = "Failed to open request";
            return resp;
        }

        std::wstring wHeaders;
        if (!contentType.empty()) {
            std::string hdr = "Content-Type: " + contentType + "\r\n";
            wHeaders = std::wstring(hdr.begin(), hdr.end());
        }

        BOOL bResults = FALSE;
        if (!binaryBody.empty()) {
            bResults = WinHttpSendRequest(hRequest, wHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wHeaders.c_str(), (DWORD)wHeaders.length(), (LPVOID)binaryBody.data(), (DWORD)binaryBody.size(), (DWORD)binaryBody.size(), 0);
        } else if (!body.empty()) {
            bResults = WinHttpSendRequest(hRequest, wHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wHeaders.c_str(), (DWORD)wHeaders.length(), (LPVOID)body.data(), (DWORD)body.length(), (DWORD)body.length(), 0);
        } else {
            bResults = WinHttpSendRequest(hRequest, wHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wHeaders.c_str(), (DWORD)wHeaders.length(), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        }

        if (bResults) {
            bResults = WinHttpReceiveResponse(hRequest, NULL);
        }

        if (bResults) {
            DWORD dwStatusCode = 0;
            DWORD dwSize = sizeof(dwStatusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
            resp.statusCode = (int)dwStatusCode;
            resp.success = (dwStatusCode >= 200 && dwStatusCode < 300);

            DWORD dwDownloaded = 0;
            do {
                dwSize = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;

                std::vector<uint8_t> buffer(dwSize);
                if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
                    resp.rawData.insert(resp.rawData.end(), buffer.begin(), buffer.begin() + dwDownloaded);
                }
            } while (dwSize > 0);

            resp.body = std::string(resp.rawData.begin(), resp.rawData.end());
        } else {
            resp.errorMessage = "WinHTTP request failed with error " + std::to_string(GetLastError());
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return resp;
    }

    static HttpResponse Get(const std::string& url, int timeoutSeconds = 10) {
        return Request("GET", url, "", "", {}, timeoutSeconds);
    }

    static HttpResponse PostJson(const std::string& url, const std::string& jsonString, int timeoutSeconds = 180) {
        return Request("POST", url, jsonString, "application/json", {}, timeoutSeconds);
    }

    static HttpResponse PostMultipartImage(const std::string& url, const std::string& filename, const std::vector<uint8_t>& imgData, bool overwrite = true, int timeoutSeconds = 60) {
        std::string boundary = "----DndMapGenBoundary" + std::to_string(GetTickCount64());
        std::string contentType = "multipart/form-data; boundary=" + boundary;

        std::string head = "--" + boundary + "\r\n";
        head += "Content-Disposition: form-data; name=\"image\"; filename=\"" + filename + "\"\r\n";
        head += "Content-Type: image/png\r\n\r\n";

        std::string mid = "\r\n--" + boundary + "\r\n";
        mid += "Content-Disposition: form-data; name=\"overwrite\"\r\n\r\n";
        mid += (overwrite ? "true" : "false");
        mid += "\r\n--" + boundary + "--\r\n";

        std::vector<uint8_t> body;
        body.insert(body.end(), head.begin(), head.end());
        body.insert(body.end(), imgData.begin(), imgData.end());
        body.insert(body.end(), mid.begin(), mid.end());

        return Request("POST", url, "", contentType, body, timeoutSeconds);
    }
};

} // namespace dnd
