﻿
#define _CRT_NON_CONFORMING_SWPRINTFS 1

#include "printing/printing.h"
#include "printing/PrintingPageHtml.h"
#include "printing/PdfiumLoad.h"
#include "printing/PrintingUtil.h"
#include "printing/PdfViewerPlugin.h"
#include "printing/PrintingSetting.h"
#include "core/mb.h"
#include "core/MbWebView.h"
#include "wke/wkedefine.h"
#include "common/ThreadCall.h"
#include "common/StringUtil.h"
#include "common/LiveIdDetect.h"
#include <iosfwd>

namespace printing {

void MB_CALL_TYPE Printing::onPrintingDocumentReadyCallback(mbWebView webView, void* param, mbWebFrameHandle frameId)
{
    Printing* self = (Printing*)param;
    self->onDocumentReady(webView);
}

void MB_CALL_TYPE Printing::onPrintingJsQueryCallback(mbWebView webView, void* param, mbJsExecState es, int64_t queryId, int customMsg, const utf8* request)
{
    Printing* self = (Printing*)param;
    self->onJsQuery(webView, es, queryId, customMsg, request);
}

BOOL MB_CALL_TYPE Printing::onPrintingLoadUrlBegin(mbWebView webView, void* param, const char* url, mbNetJob job)
{
    Printing* self = (Printing*)param;
    self->onLoadUrlBegin(webView, url, job);
    return false;
}

void MB_CALL_TYPE Printing::onPaintUpdatedCallback(mbWebView webView, void* param, const HDC hdc, int x, int y, int cx, int cy)
{
    Printing* self = (Printing*)param;
    self->onPaintUpdated(webView, hdc, x, y, cx, cy);
}

PdfDataVisitor::PdfDataVisitor(const PdfViewerPlugin* plugin)
{
    m_pdfData = *(plugin->getPdfData());
    m_pageCount = plugin->getPageCount();
    m_isPdfPluginMode = true;
    m_pdfdatas = nullptr;
}

PdfDataVisitor::PdfDataVisitor(const wkePdfDatas* pdfdatas)
{
    m_pdfdatas = pdfdatas;
    m_pageCount = m_pdfdatas->count;
    m_isPdfPluginMode = false;
}

PdfDataVisitor::~PdfDataVisitor()
{
    if (m_pdfdatas)
        wkeUtilRelasePrintPdfDatas(m_pdfdatas);
}

bool PdfDataVisitor::isValid()
{
    return true;
}

int PdfDataVisitor::getCount() const
{
    return m_pageCount;
}
       
const void* PdfDataVisitor::getData(int pageNum)
{
    if (m_isPdfPluginMode)
        return &(m_pdfData.at(0));
    return m_pdfdatas->datas[pageNum];
}

int PdfDataVisitor::getDataSize(int pageNum)
{
    if (m_isPdfPluginMode)
        return (int)m_pdfData.size();
    return (int)m_pdfdatas->sizes[pageNum];
}

//////////////////////////////////////////////////////////////////////////

int g_edgeDistance[4] = { 0/*, 1000, 0, 1000*/ };
mbDefaultPrinterSettings* s_defaultPrinterSettings = nullptr;

Printing::Printing(mbWebView webView, mbWebFrameHandle frameId)
{
    if (!s_defaultPrinterSettings)
        s_defaultPrinterSettings = new mbDefaultPrinterSettings();

    m_frameId = frameId;
    m_mbSrcView = webView;
    m_mbPreview = NULL_WEBVIEW;
    m_pdfDataVisitor = nullptr;
    m_delayRunClosure = nullptr;
    m_landscape = false;
    m_isPrintPageHeadAndFooter = false;
    m_isPrintBackgroud = false;
    m_isGettingPreviewData = false;
    m_duplex = 0;
//     m_edgeDistance[0] = 1000;
//     m_edgeDistance[1] = 1000;
//     m_edgeDistance[2] = 1000;
//     m_edgeDistance[3] = 1000;
    m_copies = 1;
    m_userSelectPaperType = -1;
    m_curPrinterSettings = new PrintSettings();
}

Printing::~Printing()
{
    if (m_isGettingPreviewData)
        DebugBreak();

    if (m_pdfDataVisitor)
        delete m_pdfDataVisitor;

    if (m_curPrinterSettings)
        delete m_curPrinterSettings;
}

static DEVMODE* createDevMode(HANDLE printer, DEVMODE* in)
{
    LONG bufferSize = ::DocumentProperties(NULL, printer, const_cast<wchar_t*>(L""), NULL, NULL, 0);
    if (bufferSize < static_cast<int>(sizeof(DEVMODE)))
        return nullptr;

    bufferSize *= 2; // Some drivers request buffers with size smaller than dmSize + dmDriverExtra. crbug.com/421402

    DEVMODE* out = reinterpret_cast<DEVMODE*>(malloc(bufferSize));
    DWORD flags = (in ? (DM_IN_BUFFER) : 0) | DM_OUT_BUFFER;
    if (::DocumentPropertiesW(NULL, printer, const_cast<wchar_t*>(L""), out, in, flags) != IDOK)
        return nullptr;

    int size = out->dmSize;
    int extra_size = out->dmDriverExtra;
    return out;
}

static void reversalWidthAndHeight(PrintSettings* settings)
{
    int temp = settings->physicalSizeDeviceUnits.h;
    settings->physicalSizeDeviceUnits.h = settings->physicalSizeDeviceUnits.w;
    settings->physicalSizeDeviceUnits.w = temp;

    temp = settings->printableAreaDeviceUnits.h;
    settings->printableAreaDeviceUnits.h = settings->printableAreaDeviceUnits.w;
    settings->printableAreaDeviceUnits.w = temp;

    temp = settings->size.h;
    settings->size.h = settings->size.w;
    settings->size.w = temp;
}

#if USING_VC6RT == 1
typedef struct _FORM_INFO_2W {
    DWORD           Flags;
    LPCWSTR         pName;
    SIZEL           Size;
    RECTL           ImageableArea;
    LPCSTR          pKeyword;
    DWORD           StringType;
    LPCWSTR         pMuiDll;
    DWORD           dwResourceId;
    LPCWSTR         pDisplayName;
    LANGID          wLangId;
} FORM_INFO_2W, * PFORM_INFO_2W, * LPFORM_INFO_2W;
#endif

//  这个获取不到paperType
static void getFormInfo(const std::wstring& devName, HANDLE handle, std::vector<FromInfo>* paperInfo)
{
    FORM_INFO_2W* formInfo2 = NULL;
    DWORD formCount = 0;
    DWORD formSizeBytes = 0;
    BOOL bSuccess = ::EnumFormsW(handle, 2, NULL, NULL, &formSizeBytes, &formCount);

    paperInfo->clear();

    if (0 < formSizeBytes) {
        formInfo2 = (FORM_INFO_2W*)malloc(formSizeBytes + formSizeBytes / sizeof(FORM_INFO_2W)); //new FORM_INFO_1W[formSizeBytes / sizeof(FORM_INFO_1W)];
        bSuccess = ::EnumFormsW(handle, 2, (PBYTE)formInfo2, formSizeBytes, &formSizeBytes, &formCount);
    }

    if (!bSuccess || 0 == formCount) {
        if (formInfo2)
            free(formInfo2);
        return;
    }

    for (size_t i = 0; i < formCount; ++i) {
        FromInfo info;
        FORM_INFO_2W* pos = formInfo2 + i;
        info.flags = pos->Flags;
        info.name = pos->pName;
        info.size = pos->Size;
        info.imageableArea = pos->ImageableArea;

        paperInfo->push_back(info);
    }

    DEVMODE* defaultDevMode = createDevMode(handle, nullptr);
    for (size_t i = 0; i < paperInfo->size(); ++i) { // 把默认设置提到最前
        std::wstring name = defaultDevMode->dmFormName;
        if (paperInfo->at(i).name == name) {
            FromInfo info = paperInfo->at(0);
            paperInfo->at(0) = paperInfo->at(i);
            paperInfo->at(i) = info;
            break;
        }
    }
    free(defaultDevMode);

    if (formInfo2)
        free(formInfo2);
}

static bool getFormInfoFromName(const std::wstring& devName, std::vector<FromInfo>* paperInfo)
{
    HANDLE handle;
    if (!::OpenPrinterW((LPWSTR)devName.c_str(), &handle, NULL)) { // ::OpenPrinter may return error but assign some value into handle.
        OutputDebugStringA("getFormInfoFromName OpenPrinterW fail\n");
        return false;
    }

    getFormInfo(devName, handle, paperInfo);
    ::ClosePrinter(handle);
    return true;
}

static bool getFormInfoFromName2(const std::wstring& devName, std::vector<FromInfo>* paperInfo, std::map<unsigned int, int>* nameToPrinterDefaultPaperType)
{
    int ret = ::DeviceCapabilities(devName.c_str(), nullptr, DC_PAPERS, nullptr, nullptr);
    if (ret < 0)
        return false;

    std::vector<WORD> paperTypes;
    paperTypes.resize(ret);
    ::DeviceCapabilities(devName.c_str(), nullptr, DC_PAPERS, (LPWSTR)paperTypes.data(), nullptr);
    
    int buffSize = ::DeviceCapabilities(devName.c_str(), NULL, DC_PAPERNAMES, NULL, NULL);
    if (buffSize != ret)
        return false;

    std::vector<wchar_t> paperNameList;
    paperNameList.resize(64 * buffSize);
    int iFlag = ::DeviceCapabilities(devName.c_str(), NULL, DC_PAPERNAMES, (LPWSTR)paperNameList.data(), NULL);
    if (iFlag < 1)
        return false;
    
    buffSize = ::DeviceCapabilities(devName.c_str(), NULL, DC_PAPERSIZE, NULL, NULL);
    if (buffSize != ret)
        return false;

    std::vector<POINT> paperSizes;
    paperSizes.resize(buffSize);
    memset(paperSizes.data(), 0, buffSize * sizeof(POINT));
    iFlag = ::DeviceCapabilities(devName.c_str(), NULL, DC_PAPERSIZE, (LPWSTR)paperSizes.data(), NULL);
    if (iFlag < 1)
        return false;

    HANDLE handle;
    if (!::OpenPrinterW((LPWSTR)devName.c_str(), &handle, NULL)) {
        OutputDebugStringA("getFormInfoFromName OpenPrinterW fail\n");
        return false;
    }

    DEVMODE* defaultDevMode = createDevMode(handle, nullptr);
    if (!defaultDevMode) {
        ::ClosePrinter(handle);
        return false;
    }

    std::wstring text = L"getFormInfoFromName2, ";
    text += devName;
    text += L":\n";
    OutputDebugStringW(text.c_str());
    for (int i = 0; i < buffSize; i++) {
        std::wstring paperName = paperNameList.data() + i * 64;
        POINT paperSize = paperSizes[i];
        WORD paperType = paperTypes[i];

        wchar_t* output = (wchar_t*)malloc(0x400);
        wsprintf(output, L"getFormInfoFromName2, paperType: %d, paperName:%ws, dmPaperSize:%d, dmFields:%d\n", paperType, paperName.c_str(), defaultDevMode->dmPaperSize, defaultDevMode->dmFields);
        OutputDebugStringW(output);
        free(output);

        FromInfo info;
        info.name = paperName;
        info.size.cx = paperSize.x * 100;
        info.size.cy = paperSize.y * 100;
        info.paperType = paperType;

        paperInfo->push_back(info);
    }
    OutputDebugStringA("getFormInfoFromName2 end\n");

    unsigned int hash = Printing::DevnameToDeviceMode::getHash(devName.c_str());
    nameToPrinterDefaultPaperType->insert(std::pair<unsigned int, int>(hash, defaultDevMode->dmPaperSize));

    for (size_t i = 0; i < paperInfo->size(); ++i) { // 把默认设置提到最前
        std::wstring dmFormName = defaultDevMode->dmFormName;
        bool modify = false;
        if (defaultDevMode->dmFields & DM_PAPERSIZE) {
            if (paperInfo->at(i).paperType == defaultDevMode->dmPaperSize)
                modify = true;
        } else if (defaultDevMode->dmFields & DM_FORMNAME) {
            if (paperInfo->at(i).name == dmFormName)
                modify = true;
        }

        if (modify) {
            FromInfo info = paperInfo->at(0);
            paperInfo->at(0) = paperInfo->at(i);
            paperInfo->at(i) = info;
            break;
        }
    }
    free(defaultDevMode);

    ::ClosePrinter(handle);

    return true;
}

static PrintSettings* getPrinterSettingsByDevName(PrintSettings* settings, bool landscape, const std::wstring& devName, const mbSize& userSelectPaperSize)
{
    HANDLE tempHandle;

    std::wstring devNameStr = recoverSlash(devName);
    //devNameStr = L"Foxit Reader PDF Printer";

    if (!::OpenPrinterW((LPWSTR)devNameStr.c_str(), &tempHandle, NULL)) { // ::OpenPrinter may return error but assign some value into handle.
        wchar_t* output = (wchar_t*)malloc(0x600 * 2);
        swprintf(output, L"Printing::getPrinterSettingsByDevName OpenPrinterW fail: %d %ws\n", ::GetLastError(), (LPWSTR)devNameStr.c_str());
        OutputDebugStringW(output);
        free(output);

        delete settings;
        return nullptr;
    }

    DEVMODE* devMode = createDevMode(tempHandle, nullptr);
    if (!devMode) {
        ::ClosePrinter(tempHandle);
        OutputDebugStringA("Printing::getPrinterSettingsByDevName createDevMode fail\n");
        delete settings;
        return nullptr;
    }
    
    HDC hdc = ::CreateDC(L"WINSPOOL", devNameStr.c_str(), NULL, devMode);
    bool isDevLandscape = devMode->dmOrientation == DMORIENT_LANDSCAPE;
    free(devMode);
    if (!hdc) {
        ::ClosePrinter(tempHandle);
        OutputDebugStringA("Printing::getPrinterSettingsByDevName CreateDC fail\n");
        delete settings;
        return nullptr;
    }

    settings->dpi = ::GetDeviceCaps(hdc, LOGPIXELSX); // device_units_per_inch
    settings->physicalSizeDeviceUnits = { ::GetDeviceCaps(hdc, PHYSICALWIDTH), ::GetDeviceCaps(hdc, PHYSICALHEIGHT) };
    settings->printableAreaDeviceUnits = {
        ::GetDeviceCaps(hdc, PHYSICALOFFSETX), ::GetDeviceCaps(hdc, PHYSICALOFFSETY),
        ::GetDeviceCaps(hdc, HORZRES), ::GetDeviceCaps(hdc, VERTRES) 
    };
    if (0 == settings->dpi) {
        ::DeleteDC(hdc);
        ::ClosePrinter(tempHandle);
        OutputDebugStringA("Printing::getPrinterSettingsByDevName GetDeviceCaps fail\n");
        delete settings;
        return nullptr;
    }

    char* output = (char*)malloc(0x200);
    sprintf(output, "getPrinterSettingsByDevName, physicalSize:(%d %d) userSelect:(%d %d) printableArea:(%d %d %d %d)\n",
        settings->physicalSizeDeviceUnits.w, settings->physicalSizeDeviceUnits.h,
        userSelectPaperSize.w, userSelectPaperSize.h,
        settings->printableAreaDeviceUnits.x, settings->printableAreaDeviceUnits.y,
        settings->printableAreaDeviceUnits.w, settings->printableAreaDeviceUnits.h);
    OutputDebugStringA(output);
    free(output);

    // Sanity check the printable_area: we've seen crashes caused by a printable area rect of 0, 0, 0, 0, so it seems some drivers don't set it.
    if ((!settings->printableAreaDeviceUnits.w || !settings->printableAreaDeviceUnits.h) || !isContains(settings->physicalSizeDeviceUnits, settings->printableAreaDeviceUnits))
        settings->printableAreaDeviceUnits = { 0, 0, settings->physicalSizeDeviceUnits.w, settings->physicalSizeDeviceUnits.h };

    double kThousandthsOfMillimeterInInch = 25400;
    mbSize paperSizeA4 = { (int)(kA4WidthInch * settings->dpi), (int)(kA4HeightInch * settings->dpi) };
    settings->size = {
        (int)((userSelectPaperSize.w / kThousandthsOfMillimeterInInch) * settings->dpi),
        (int)((userSelectPaperSize.h / kThousandthsOfMillimeterInInch) * settings->dpi)
    };

    if (isDevLandscape)
        reversalWidthAndHeight(settings);

    if (settings->size.w > settings->printableAreaDeviceUnits.w)
        settings->size.w = settings->printableAreaDeviceUnits.w;

    if (settings->size.h > settings->printableAreaDeviceUnits.h)
        settings->size.h = settings->printableAreaDeviceUnits.h;

    if (landscape)
        reversalWidthAndHeight(settings);

    ::DeleteDC(hdc);
    ::ClosePrinter(tempHandle);
    return settings;
}

bool Printing::getPdfDataFromPdfViewerInBlinkThread(int64_t queryId)
{
    mb::MbWebView* webview = (mb::MbWebView*)common::LiveIdDetect::get()->getPtr((int64_t)m_mbSrcView);

    PdfViewerPlugin* plugin = (PdfViewerPlugin*)wkeGetUserKeyValue(webview->getWkeWebView(), "ChildPdfViewerPlugin");
    if (!plugin)
        return false;

    if (m_pdfDataVisitor)
        delete m_pdfDataVisitor;
    m_pdfDataVisitor = nullptr;

    if (plugin->getPageCount()) {
        m_pdfDataVisitor = new PdfDataVisitor(plugin);
        mbResponseQuery(m_mbPreview, queryId, plugin->getPageCount(), nullptr);
    } else {
        std::string msgIfFail = common::utf16ToUtf8(L"获取PDFViewer页数为0！");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
    }

    return true;
}

void Printing::getPdfDataInBlinkThread(int64_t queryId, const std::string& printerName)
{
    OutputDebugStringA("Printing::getPdfDataInBlinkThread 1\n");

    if (m_curPrinterSettings)
        delete m_curPrinterSettings;
    m_curPrinterSettings = getPrinterSettingsByDevName((new PrintSettings()), m_landscape, common::utf8ToUtf16(printerName), m_userSelectPaperSize);
    if (!m_curPrinterSettings) {
        std::string msgIfFail = common::utf16ToUtf8(L"打印参数错误！！");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
        return;
    }

    if (getPdfDataFromPdfViewerInBlinkThread(queryId))
        return;

    const int dpi = m_curPrinterSettings->dpi; // 这个dpi，在打印机上又叫device_units_per_inch
    const int unitsPerInch = dpi;
    const int headerFooterTextHeight = (int)convertUnitDouble(kSettingHeaderFooterInterstice, kPointsPerInch, unitsPerInch); // Hard-code text_height = 0.5cm = ~1/5 of inch.
    const int marginPrinterUnits = convertUnit(1000, kHundrethsMMPerInch, unitsPerInch); // Default margins 1.0cm = ~2/5 of an inch.

    wkePrintSettings params;
    params.structSize = sizeof(wkePrintSettings);
    
    params.width = m_curPrinterSettings->size.w; // in px or device context
    params.height = m_curPrinterSettings->size.h;
    params.marginLeft = convertUnit(g_edgeDistance[0], kHundrethsMMPerInch, unitsPerInch); // marginPrinterUnits
    params.marginTop = convertUnit(g_edgeDistance[1], kHundrethsMMPerInch, unitsPerInch);
    params.marginRight = convertUnit(g_edgeDistance[2], kHundrethsMMPerInch, unitsPerInch);
    params.marginBottom = convertUnit(g_edgeDistance[3], kHundrethsMMPerInch, unitsPerInch);
    params.dpi = dpi;
    params.isPrintPageHeadAndFooter = m_isPrintPageHeadAndFooter;
    params.isPrintBackgroud = m_isPrintBackgroud;
    params.isLandscape = m_landscape;

    char* output = (char*)malloc(0x200);
    sprintf(output, "Printing::getPdfDataInBlinkThread: userSelect:(%d %d) %d, physicalSize:(%d %d) printableArea:(%d %d %d %d)\n", 
        params.width, params.height, dpi,
        m_curPrinterSettings->physicalSizeDeviceUnits.w, m_curPrinterSettings->physicalSizeDeviceUnits.h,
        m_curPrinterSettings->printableAreaDeviceUnits.x, m_curPrinterSettings->printableAreaDeviceUnits.y,
        m_curPrinterSettings->printableAreaDeviceUnits.w, m_curPrinterSettings->printableAreaDeviceUnits.h);
    OutputDebugStringA(output);
    free(output);

    if (m_pdfDataVisitor)
        delete m_pdfDataVisitor;

    mb::MbWebView* webview = (mb::MbWebView*)common::LiveIdDetect::get()->getPtr((int64_t)m_mbSrcView);
    const wkePdfDatas* pdfDatas = wkeUtilPrintToPdf(webview->getWkeWebView(), (wkeWebFrameHandle)m_frameId, &params);
    if (!pdfDatas || 0 == pdfDatas->count) {
        OutputDebugStringA("Printing::getPdfDataInBlinkThread fail 1\n");

        std::string msgIfFail = common::utf16ToUtf8(L"生成打印预览数据失败！");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
        return;
    }
    m_pdfDataVisitor = new PdfDataVisitor(pdfDatas);

    ///----
//     if (0) {
//         HANDLE hFile = CreateFileW(L"d:\\test.pdf", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
//         if (INVALID_HANDLE_VALUE == hFile) {
//             ::MessageBoxW(nullptr, L"失败", L"打开文件失败", 0);
//             return;
//         }
// 
//         DWORD numberOfBytesWrite = 0;
//         BOOL b = ::WriteFile(hFile, pdfDatas->datas[0], pdfDatas->sizes[0], &numberOfBytesWrite, nullptr);
//         ::CloseHandle(hFile);
//     }
    ///----

    OutputDebugStringA("Printing::getPdfDataInBlinkThread over\n");
    mbResponseQuery(m_mbPreview, queryId, m_pdfDataVisitor->getCount(), nullptr);
}

static void createJsToSetDefaultParam(mbWebView preview)
{
    std::string jscript = "window.setDefaultParam({";
//     for (size_t i = 0; i < 4; ++i) {
//         char temp[32] = { 0 };
//         sprintf_s(temp, 31, "%d, ", g_edgeDistance[i] / 100);
//         jscript += temp;
//     }
    std::vector<char> temp(1000);
    sprintf(temp.data(), 
        "edgeDistanceLeft: %d, edgeDistanceTop: %d, edgeDistanceRight: %d, edgeDistanceBottom: %d, "
        "isLandscape: %s, isPrintHeadFooter: %s, isPrintBackgroud: %s,"
        "copies: %d, paperType: %d",
        s_defaultPrinterSettings->edgeDistanceLeft,
        s_defaultPrinterSettings->edgeDistanceTop,
        s_defaultPrinterSettings->edgeDistanceRight,
        s_defaultPrinterSettings->edgeDistanceBottom,
        s_defaultPrinterSettings->isLandscape ? "true" : "false",
        s_defaultPrinterSettings->isPrintHeadFooter ? "true" : "false",
        s_defaultPrinterSettings->isPrintBackgroud ? "true" : "false",
        s_defaultPrinterSettings->copies, 
        s_defaultPrinterSettings->paperType
        );
    jscript += temp.data();
    jscript += "});";
    mbRunJs(preview, mbWebFrameGetMainFrame(preview), jscript.c_str(), true, nullptr, nullptr, nullptr);
}

static void createJsToAddPrinter(mbWebView preview, const std::vector<std::wstring>& printerNames, std::map<unsigned int, int>* nameToPrinterDefaultPaperType)
{
    std::wstring jscript = L"window.addPrinter([";
    for (size_t i = 0; i < printerNames.size(); ++i) {
        std::wstring printerName = printerNames[i];
        
        std::vector<FromInfo> paperInfo;
        getFormInfoFromName2(printerName, &paperInfo, nameToPrinterDefaultPaperType);
        if (0 == paperInfo.size())
            continue;

        printerName = trimQuote(printerName);

        jscript += L"{\"text\":\"";
        jscript += cutOffTitle(printerName.c_str());
        jscript += L"\", \"value\":\"";
        jscript += trimSlash(printerName);
        jscript += L"\", \"paperInfo\":[\n";

        for (size_t j = 0; j < paperInfo.size(); ++j) {
            jscript += L"{\"name\":\"";

            std::wstring paperName = trimQuote(paperInfo[j].name);
            jscript += paperName.c_str();

            wchar_t temp[0x100];
            wsprintf(temp, L"\", \"width\":%d, \"height\":%d, \"paperType\":%d", paperInfo[j].size.cx, paperInfo[j].size.cy, paperInfo[j].paperType);
            jscript += temp;

            jscript += L"},\n";
        }
        jscript += L"]},\n";
    }

    jscript += L"]);";
    std::string jscriptA = common::utf16ToUtf8(jscript.c_str());
    mbRunJs(preview, mbWebFrameGetMainFrame(preview), jscriptA.c_str(), true, nullptr, nullptr, nullptr);
    mbRunJs(preview, mbWebFrameGetMainFrame(preview), "console.log('createJsToAddPrinter end')", true, nullptr, nullptr, nullptr);
}

bool Printing::enumNetworkPrinters(std::vector<std::wstring>* printerNames)
{
    DWORD enumFlags = PRINTER_ENUM_NETWORK | PRINTER_ENUM_LOCAL | PRINTER_ENUM_REMOTE;
    DWORD level = 2;

    DWORD bytesNeeded = 0;
    DWORD countReturned = 0;
    (void)::EnumPrintersW(enumFlags, NULL, level, NULL, 0, &bytesNeeded, &countReturned);
    if (!bytesNeeded)
        return false;

    BYTE* printerInfoBuffer = new BYTE[bytesNeeded];
    BOOL ret = ::EnumPrintersW(enumFlags, NULL, level, printerInfoBuffer, bytesNeeded, &bytesNeeded, &countReturned);
    if (!(ret && countReturned)) // have printers Open the first successfully found printer.
        return false;

    const PRINTER_INFO_2* info = reinterpret_cast<PRINTER_INFO_2*>(printerInfoBuffer);
    const PRINTER_INFO_2* infoEnd = info + countReturned;

    OutputDebugStringA("Printing::enumNetworkPrinters:\n");
    for (; info < infoEnd; ++info) {
        m_nameToDeviceMode.add(info);

        std::wstring printerName = info->pPrinterName;
        printerNames->push_back(printerName);

        printerName += L"\n";
        OutputDebugStringW(printerName.c_str());
    }

    delete printerInfoBuffer;
    OutputDebugStringA("Printing::enumNetworkPrinters end\n");
    return true;
}

bool Printing::enumLocalPrinters(std::vector<std::wstring>* printerNames)
{
    OutputDebugStringA("EnumPrintersW 1\n");
    DWORD enumFlags = enumFlags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
    DWORD level = 2;

    DWORD bytesNeeded = 0;
    DWORD countReturned = 0;
    (void)::EnumPrintersW(enumFlags, NULL, level, NULL, 0, &bytesNeeded, &countReturned);
    if (!bytesNeeded)
        return false;    
    
    BYTE* printerInfoBuffer = new BYTE[bytesNeeded];
    BOOL ret = ::EnumPrintersW(enumFlags, NULL, level, printerInfoBuffer, bytesNeeded, &bytesNeeded, &countReturned);
    if (!(ret && countReturned)) { // have printers Open the first successfully found printer.
        OutputDebugStringA("EnumPrintersW fail\n");
        return false;
    }
    OutputDebugStringA("EnumPrintersW ok\n");

    const PRINTER_INFO_2* info = reinterpret_cast<PRINTER_INFO_2*>(printerInfoBuffer);
    const PRINTER_INFO_2* infoEnd = info + countReturned;

    OutputDebugStringA("Printing::enumLocalPrinters:\n");
    for (; info < infoEnd; ++info) {
        m_nameToDeviceMode.add(info);

        std::wstring printerName = info->pPrinterName;
        printerNames->push_back(printerName);

        printerName += L"\n";
        OutputDebugStringW(printerName.c_str());
    }

    delete printerInfoBuffer;
    OutputDebugStringA("Printing::enumLocalPrinters end\n");
    return true;
}

bool Printing::enumPrinters()
{
    m_nameToDeviceMode.clear();
    std::vector<std::wstring> printerNames;
    enumLocalPrinters(&printerNames);
    enumNetworkPrinters(&printerNames);

    std::vector<wchar_t> buffer;
    DWORD bufferSize = MAX_PATH * 2;
    buffer.resize(bufferSize);

    bool hasDefaultPrinter = false;
    BOOL b = ::GetDefaultPrinter(&buffer.at(0), &bufferSize);
    if (b && 2 <= bufferSize)
        hasDefaultPrinter = true;
    std::wstring defaultPrinter(&buffer.at(0), bufferSize - 1);

    if (hasDefaultPrinter) { // 排序
        for (size_t i = 0; i < printerNames.size(); ++i) {
            std::wstring printerName = printerNames.at(i);
            if (printerName == defaultPrinter && 0 != i) {
                std::wstring name = printerNames[0];
                printerNames.at(0) = printerName;
                printerNames.at(i) = name;
                break;
            }
        }
    }

    createJsToSetDefaultParam(m_mbPreview);
    createJsToAddPrinter(m_mbPreview, printerNames, &m_nameToPrinterDefaultPaperType);
    return true;
}

void Printing::onDocumentReady(mbWebView webView)
{
    enumPrinters();
    mbRunJs(m_mbPreview, mbWebFrameGetMainFrame(m_mbPreview), "toNativeGetPreview();", true, nullptr, nullptr, nullptr);
}

static void simpleModifyWorldTransform(HDC context, int offsetX, int offsetY, float shrink_factor)
{
    XFORM xform = { 0 };
    xform.eDx = static_cast<float>(offsetX);
    xform.eDy = static_cast<float>(offsetY);
    xform.eM11 = xform.eM22 = 1.f / shrink_factor;
    BOOL res = ::ModifyWorldTransform(context, &xform, MWT_LEFTMULTIPLY);
}

void Printing::doPrintImpl(const std::vector<int>& needPrintedPages, DEVMODE* devMode, const std::wstring& devName)
{
//     HDC hdc = TestGetPrinterDC(devMode);
//     ::DeleteDC(hdc);

    HDC hdcOfPrinter = ::CreateDC(L"WINSPOOL", devName.c_str(), NULL, devMode);

    char* output = (char*)malloc(0x100);
    sprintf(output, "Printing::doPrintImpl, devMode->dmPaperSize: %d\n", devMode->dmPaperSize);
    OutputDebugStringA(output);
    free(output);

    DOCINFO di;
    di.cbSize = sizeof(DOCINFO);
    di.lpszDocName = devName.c_str();
    di.lpszOutput = NULL;
    ::StartDoc(hdcOfPrinter, &di);

    ::SetMapMode(hdcOfPrinter, MM_TEXT);

    int offsetX = ::GetDeviceCaps(hdcOfPrinter, PHYSICALOFFSETX);
    int offsetY = ::GetDeviceCaps(hdcOfPrinter, PHYSICALOFFSETY);
    int dpi = ::GetDeviceCaps(hdcOfPrinter, LOGPIXELSX);
    float scaleX = 1;
    float scaleY = 1;

    int diffX = (m_curPrinterSettings->physicalSizeDeviceUnits.w - m_curPrinterSettings->size.w) / 2;
    int diffY = (m_curPrinterSettings->physicalSizeDeviceUnits.h - m_curPrinterSettings->size.h) / 2;

    output = (char*)malloc(0x100);
    sprintf(output, "Printing::doPrintImpl: %d %d, %d %d, %d\n", offsetX, offsetY, diffX, diffY, m_curPrinterSettings->printableAreaDeviceUnits.w);
    OutputDebugStringA(output);
    free(output);

    int savedState = SaveDC(hdcOfPrinter);
    initializeDC(hdcOfPrinter);

    for (int i = 0; i < m_pdfDataVisitor->getCount(); ++i) {
        if (0 == needPrintedPages[i])
            continue;
        if (i > kMaxPrintPageNum)
            break;
        int savedState2 = SaveDC(hdcOfPrinter);

        //simpleModifyWorldTransform(hdcOfPrinter, (diffX - offsetX) / 1, (diffY- offsetY) / 1, 1);

        ::StartPage(hdcOfPrinter);

        BOOL b = PdfiumLoad::get()->renderPDFPageToDC(m_pdfDataVisitor->getData(i),
            m_pdfDataVisitor->getDataSize(i),
            m_pdfDataVisitor->isPdfPluginMode() ? i : 0,
            hdcOfPrinter, dpi,
            0, 0,
            (int)(m_curPrinterSettings->physicalSizeDeviceUnits.w * scaleX),
            (int)(m_curPrinterSettings->physicalSizeDeviceUnits.h * scaleY),
            true /*fit_to_bounds*/, true /*stretch_to_bounds*/, false, false, false);

        mb::MbWebView* webview = (mb::MbWebView*)common::LiveIdDetect::get()->getPtr((int64_t)m_mbSrcView);
        if (webview->m_printingCallback) {
            mbPrintintSettings settings;
            settings.dpi = dpi;
            settings.width = m_curPrinterSettings->size.w;
            settings.height = m_curPrinterSettings->size.h;
            settings.scale = 1;
            webview->m_printingCallback(m_mbSrcView, webview->m_printingCallbackParam, kPrintintStepPrinting, hdcOfPrinter, &settings, i);
        }

//         if (0) {
//             wchar_t* text = L"打印测试文本";
// 
//             float xScale = getXScale(hdcOfPrinter);
//             float yScale = getYScale(hdcOfPrinter);
// 
//             double x = cm2Unit_W(xScale, 0); //在（2.5cm， 2.5cm）处打印文本  
//             double y = cm2Unit_H(yScale, 2.5);
// 
//             ::TextOutW(hdcOfPrinter, (int)x, (int)y, (LPWSTR)text, wcslen(text));
// 
//             HPEN hPen = ::CreatePen(PS_SOLID, 2, RGB(55, 66, 88));
//             ::SelectObject(hdcOfPrinter, hPen);
//             drawRect(hdcOfPrinter, 0, 0, m_curPrinterSettings->printableAreaDeviceUnits.w, m_curPrinterSettings->printableAreaDeviceUnits.h, 10); // 可打印区域
//             ::DeleteObject(hPen);
//         }
        //drawPage(hdcOfPrinter, 1);

        ::EndPage(hdcOfPrinter);

        BOOL res = RestoreDC(hdcOfPrinter, savedState2);
    }

    int res = RestoreDC(hdcOfPrinter, savedState);
    ::EndDoc(hdcOfPrinter);
    ::DeleteDC(hdcOfPrinter);
}

void Printing::doPrint(const std::string& printerParams, int64_t queryId)
{
    std::string msgIfFail;
    if (!m_pdfDataVisitor) {
        msgIfFail = common::utf16ToUtf8(L"请先点击预览生成打印数据");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
        return;
    }

    if (0 == m_pdfDataVisitor->getCount()) {
        msgIfFail = common::utf16ToUtf8(L"可打印页数为0");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
        return;
    }
   
    std::string printerName;
    std::vector<std::pair<int, int>> pagesCount;

    bool b = parseDoPrintParams(printerParams.c_str(), &pagesCount, &printerName, &m_copies);
    if (!b) {
        msgIfFail = common::utf16ToUtf8(L"打印页码填写错误");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
        return;
    }
    std::vector<int> needPrintedPages = createNeedPrintedPages(m_pdfDataVisitor->getCount(), pagesCount);
    
    std::wstring devName = common::utf8ToUtf16(printerName);
    devName = recoverSlash(devName);
    DEVMODE* devMode = m_nameToDeviceMode.find(devName);
    if (!devMode) {
        msgIfFail = common::utf16ToUtf8(L"打开打印设备失败(2)");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
        return;
    }

    HANDLE handlePrinter;
    if (!::OpenPrinterW((LPWSTR)devName.c_str(), &handlePrinter, NULL)) { // ::OpenPrinter may return error but assign some value into handle.
        msgIfFail = common::utf16ToUtf8(L"打开打印设备失败(3)");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
        return;
    }

    if (m_landscape) {
        devMode->dmFields |= DM_ORIENTATION;
        devMode->dmOrientation = true ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
    }
    devMode->dmFields |= (DM_PAPERSIZE /*| DM_PAPERWIDTH | DM_PAPERLENGTH |*/);

    unsigned int hash = DevnameToDeviceMode::getHash(devName.c_str());
    std::map<unsigned int, int>::iterator it = m_nameToPrinterDefaultPaperType.find(hash);
    
    if (it != m_nameToPrinterDefaultPaperType.end())
        devMode->dmPaperSize = it->second; //  HP打印机似乎不能选择，只能用这个值
    else
        devMode->dmPaperSize = m_userSelectPaperType;
//     devMode->dmPaperWidth = m_userSelectPaperSize.w / 100; // 这里会影响打印出来的pdf的大小
//     devMode->dmPaperLength = m_userSelectPaperSize.h / 100;
    if (m_copies > 1) {
        devMode->dmFields |= DM_COPIES;
        devMode->dmCopies = m_copies;
    }

    if (m_duplex == 1)
        devMode->dmDuplex = DMDUP_HORIZONTAL;
    else if (m_duplex == 2)
        devMode->dmDuplex = DMDUP_VERTICAL;
    else
        devMode->dmDuplex = DMDUP_SIMPLEX;

    LONG ret = ::DocumentProperties(nullptr, handlePrinter, (LPWSTR)devName.c_str(), devMode, devMode, DM_IN_BUFFER | DM_OUT_BUFFER);
    if (ret == IDOK)
        doPrintImpl(needPrintedPages, devMode, devName);
    else {
        msgIfFail = common::utf16ToUtf8(L"打开打印设备失败(4)");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
    }

    ::ClosePrinter(handlePrinter);

    mbResponseQuery(m_mbPreview, queryId, 1, nullptr);

    mbRunJs(m_mbSrcView, mbWebFrameGetMainFrame(m_mbSrcView), "$('#bill_print').hide();", true, nullptr, nullptr, nullptr);
}

void Printing::onPaintUpdated(mbWebView webView, const HDC hdc, int x, int y, int cx, int cy)
{
    if (m_delayRunClosure) {
        char* output = (char*)malloc(0x100);
        sprintf(output, "Printing::onPaintUpdated: %d %d\n", cx, cy);
        OutputDebugStringA(output);
        free(output);
    }
    if (m_delayRunClosure && cx > 800 && cy > 600) {
        common::ThreadCall::callBlinkThreadAsync(MB_FROM_HERE, std::move(*m_delayRunClosure));
        delete m_delayRunClosure;
        m_delayRunClosure = nullptr;
    }
}

const int kGetPreviewMsg = 2;
const int kDoPrintMsg = 3;
const int kDoCloseMsg = 4;

std::string* Printing::parseGetPreviewParams(const utf8* request)
{
    std::string* printerName = nullptr;
    std::string requestStr(request);
    if (requestStr.size() <= 6 || ',' != requestStr[1])
        return nullptr;
    m_landscape = ('1' == requestStr[0]);
    m_isPrintPageHeadAndFooter = ('1' == requestStr[2]);
    m_isPrintBackgroud = ('1' == requestStr[4]);

    if ('0' == requestStr[6])
        m_duplex = 0;
    else if ('1' == requestStr[6])
        m_duplex = 1;
    else
        m_duplex = 2;

    requestStr = requestStr.substr(8);

    size_t pos = requestStr.find("###");
    if (std::string::npos == pos)
        return new std::string(requestStr);

    std::string* devName = new std::string(requestStr.substr(0, pos));
    requestStr = requestStr.substr(pos + 3);

    for (int i = 0; i < 7; ++i) {
        pos = requestStr.find(",");
        if (std::string::npos == pos)
            break;
        std::string numStr = requestStr.substr(0, pos);
        requestStr = requestStr.substr(pos + 1);

        int num = atoi(numStr.c_str());

        if (i < 4) {
            if (num < 0 || num > 100)
                continue;
            g_edgeDistance[i] = num * 100;
        } else if (4 == i) {
            m_userSelectPaperSize.w = num;
        } else if (5 == i) {
            m_userSelectPaperSize.h = num;
        } else if (6 == i) {
            m_userSelectPaperType = num;
        }
    }
    return devName;
}

void Printing::getPreview(int64_t queryId, int64_t id, const utf8* request)
{
    OutputDebugStringA("Printing::getPreview 1\n");
    if (m_delayRunClosure) {
        OutputDebugStringA("Printing::getPreview fail 1\n");
        return;
    }

    if (!PdfiumLoad::get()) {
        OutputDebugStringA("Printing::getPreview fail 2\n");

        std::string msgIfFail = common::utf16ToUtf8(L"缺少pdfium.dll");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
        return;
    }

    std::string* printerName = parseGetPreviewParams(request);
    if (!printerName) {
        OutputDebugStringA("Printing::getPreview fail 2\n");

        std::string msgIfFail = common::utf16ToUtf8(L"打印参数错误！");
        mbResponseQuery(m_mbPreview, queryId, 0, msgIfFail.c_str());
        return;
    }

    int64_t previewId = m_mbPreview;
    mb::MbWebView* mbPreview = (mb::MbWebView*)common::LiveIdDetect::get()->getPtr((int64_t)m_mbPreview);
    
    Printing* self = this;
    mbPreview->setIsMouseKeyMessageEnable(false);

    m_delayRunClosure = new std::function<void(void)>([id, previewId, self, queryId, printerName] {
        OutputDebugStringA("Printing::getPreview lambda\n");

        mb::MbWebView* mbSrcView = (mb::MbWebView*)common::LiveIdDetect::get()->getPtrLocked(id);
        if (!mbSrcView) {
            OutputDebugStringA("Printing::getPreview fail 3\n");

            delete printerName;
            return;
        }

        mb::MbWebView* mbPreview = (mb::MbWebView*)common::LiveIdDetect::get()->getPtrLocked(previewId);
        if (!mbPreview) {
            OutputDebugStringA("Printing::getPreview fail 4\n");

            common::LiveIdDetect::get()->unlock(id, mbSrcView);
            delete printerName;
            return;
        }
        self->m_isGettingPreviewData = true;
        self->getPdfDataInBlinkThread(queryId, *printerName);
        mbPreview->setIsMouseKeyMessageEnable(true);
        delete printerName;
        self->m_isGettingPreviewData = false;

        common::LiveIdDetect::get()->unlock(id, mbSrcView);
        common::LiveIdDetect::get()->unlock(previewId, mbPreview);
    });
}

void Printing::onJsQuery(mbWebView webView, mbJsExecState es, int64_t queryId, int customMsg, const utf8* request)
{
    int64_t id = m_mbSrcView;
    Printing* self = this;

    HWND hWnd = mbGetHostHWND(webView);
    
    if (kGetPreviewMsg == customMsg) {
        if (common::LiveIdDetect::get()->isLive(id))
            getPreview(queryId, id, request);
    } else if (kDoPrintMsg == customMsg) {
        doPrint(request, queryId);
    } else if (kDoCloseMsg == customMsg) {
        ::PostMessage(hWnd, WM_CLOSE, 0, 0);

        int64_t srcId = m_mbSrcView;
        mb::MbWebView* srcView = (mb::MbWebView*)common::LiveIdDetect::get()->getPtrLocked(srcId);
        if (!srcView)
            return;
        srcView->setIsMouseKeyMessageEnable(true);
        common::LiveIdDetect::get()->unlock(srcId, srcView);
    }
}

void Printing::onLoadUrlBegin(mbWebView webView, const char* url, mbNetJob job)
{
    const char token[] = "print://img_src_";
    std::string urlStr(url);
    if (-1 != urlStr.find(token)) {
        std::string previewSrc = urlStr.substr(sizeof(token) - 1);
        size_t pos = previewSrc.find('?');
        if (-1 == pos)
            return;
        previewSrc = previewSrc.substr(0, pos);

        int64_t srcId = m_mbSrcView;
        mb::MbWebView* srcView = (mb::MbWebView*)common::LiveIdDetect::get()->getPtrLocked(srcId);
        if (!srcView)
            return;
        onGetPrintPreviewSrc(previewSrc, job);
        common::LiveIdDetect::get()->unlock(srcId, srcView);
    }
}

void Printing::onGetPrintPreviewSrc(const std::string& urlStr, mbNetJob job)
{
    int count = atoi(urlStr.c_str());
    if (count > m_pdfDataVisitor->getCount() || count > kMaxPrintPageNum)
        return;

    const void* pdfData = m_pdfDataVisitor->getData(count);
    size_t pdfDataSize = m_pdfDataVisitor->getDataSize(count);

    mb::MbWebView* webview = (mb::MbWebView*)common::LiveIdDetect::get()->getPtr((int64_t)m_mbSrcView);

    HWND hWnd = webview->getHostWnd();
    HDC refDeviceContext = ::GetDC(hWnd);
    int desiredDpi = ::GetDeviceCaps(refDeviceContext, LOGPIXELSX);
    ::ReleaseDC(hWnd, refDeviceContext);
    RenderSettings renderSettings = { desiredDpi, false, false, false, false, false };

    int pageNum = m_pdfDataVisitor->isPdfPluginMode() ? count : 0;

    std::vector<char>* bitmap = PrintingUtil::renderPdfPageToBitmap(m_mbSrcView, hWnd, pageNum, *m_curPrinterSettings, renderSettings, pdfData, pdfDataSize);
    mbNetSetData(job, bitmap->data(), (int)bitmap->size());
    delete bitmap;
}

BOOL MB_CALL_TYPE Printing::onDestroyCallback(mbWebView webView, void* param, void* unuse)
{
    Printing* self = (Printing*)param;
    mb::MbWebView* webview = (mb::MbWebView*)common::LiveIdDetect::get()->getPtr((int64_t)self->m_mbSrcView);
    webview->m_printing = nullptr;
    delete self;
    return TRUE;
}

void Printing::createPreviewWin()
{
    HWND hWnd = nullptr;
    if (m_mbPreview) {
        hWnd = mbGetHostHWND(m_mbPreview);
        SetForegroundWindow(hWnd);
        return;
    }
     
    HWND hSrcWnd = mbGetHostHWND(m_mbSrcView);

    m_mbPreview = mbCreateWebCustomWindow(hSrcWnd, WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 220, 220, 1, 1);
    hWnd = mbGetHostHWND(m_mbPreview);

    ::SetWindowTextW(hWnd, L"打印");
    mbOnDocumentReady(m_mbPreview, onPrintingDocumentReadyCallback, this);
    mbOnJsQuery(m_mbPreview, onPrintingJsQueryCallback, this);
    mbOnPaintUpdated(m_mbPreview, onPaintUpdatedCallback, this);
    mbOnLoadUrlBegin(m_mbPreview, onPrintingLoadUrlBegin, this);
    mbOnDestroy(m_mbPreview, onDestroyCallback, this);

    mbRunJs(m_mbSrcView, mbWebFrameGetMainFrame(m_mbSrcView), "$('#bill_print').show();", true, nullptr, nullptr, nullptr);

    //mbLoadURL(m_mbPreview, "file:///G:/mycode/mb/mbvip/printing/PrintingPage.htm");
    mbLoadHtmlWithBaseUrl(m_mbPreview, (const utf8 *)kPrintingPageHtml, "PrintingPage.html");
     
    DWORD dwStyle = ::GetWindowLong(hWnd, GWL_STYLE);
    dwStyle = dwStyle & (~WS_CAPTION) & (~WS_SYSMENU) & (~WS_SIZEBOX);
    ::SetWindowLong(hWnd, GWL_STYLE, dwStyle);

    mbResize(m_mbPreview, 1285, 680);
    mbMoveToCenter(m_mbPreview);
    mbShowWindow(m_mbPreview, TRUE);
    ::UpdateWindow(hWnd);
}

void Printing::run(const mbPrintSettings* printParams)
{
    createPreviewWin();
}

}