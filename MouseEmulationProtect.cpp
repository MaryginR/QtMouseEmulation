#include "MouseEmulationProtectHeaders.h"

std::chrono::steady_clock::time_point LastMouseMessage;
std::chrono::steady_clock::time_point LastRawInputClick;
bool EmulatingDetected = false;
WINDOWPLACEMENT lastwp;

// Функция для проверки состояния окна (перетаскивается ли оно или изменяются размеры)
bool IsWindowDraggingOrResizing(HWND hwnd) {
    WINDOWPLACEMENT wp;
    wp.length = sizeof(WINDOWPLACEMENT);

    if (GetWindowPlacement(hwnd, &wp)) {
        bool isMoved = (wp.rcNormalPosition.left != lastwp.rcNormalPosition.left ||
            wp.rcNormalPosition.top != lastwp.rcNormalPosition.top);

        bool isResized = ((wp.rcNormalPosition.right - wp.rcNormalPosition.left) !=
            (lastwp.rcNormalPosition.right - lastwp.rcNormalPosition.left)) ||
            ((wp.rcNormalPosition.bottom - wp.rcNormalPosition.top) !=
                (lastwp.rcNormalPosition.bottom - lastwp.rcNormalPosition.top));

        if (isMoved || isResized) {
            lastwp = wp;
            return true;
        }
    }
    return false;
}

// Функция проверки движения курсора
static void CheckCursor(HWND hwnd)
{
    POINT CurrentCursorPos, LastCursorPos;
    std::chrono::steady_clock::time_point LastDraggingOrResizing = std::chrono::steady_clock::now();

    GetWindowPlacement(hwnd, &lastwp);
    GetCursorPos(&LastCursorPos);

    while (true)
    {
        GetCursorPos(&CurrentCursorPos);

        if (IsWindowDraggingOrResizing(hwnd))
        {
            LastDraggingOrResizing = std::chrono::steady_clock::now();
        }

        int deltaX = CurrentCursorPos.x - LastCursorPos.x;
        int deltaY = CurrentCursorPos.y - LastCursorPos.y;

        if (deltaX != 0 || deltaY != 0)
        {
            auto msFromLastMessage = std::chrono::steady_clock::now();

            auto elapsedTicks = std::chrono::duration_cast<std::chrono::milliseconds>(msFromLastMessage - LastMouseMessage);
            auto elapsedTicksDraggingOrResizing = std::chrono::duration_cast<std::chrono::milliseconds>(msFromLastMessage - LastDraggingOrResizing);

            if (elapsedTicks.count() > 200 && elapsedTicksDraggingOrResizing.count() > 200)
            {
                EmulatingDetected = true;
            }

            LastCursorPos = CurrentCursorPos;
        }

        Sleep(1);
    }
}

// Подмененная оконная процедура для перехвата сообщений(сюда попадают все сообщения посланные в оконную процедуру, в отличие от фильтра для raw пакетов)
LRESULT CALLBACK SubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    if (uMsg == WM_LBUTTONDOWN) {
        auto elapsedTicks = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - LastMouseMessage);
        if (elapsedTicks.count() > 200) {
            EmulatingDetected = true;
        }
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Функция для установки подмены оконной процедуры
void SubclassWindow(HWND hwnd) {
    if (!SetWindowSubclass(hwnd, SubclassProc, 1, 0)) {
        throw "Failed to subclass window";
    }
}

// Класс для обработки нативных событий Windows
class RawInputEventFilter : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override {
        if (eventType == "windows_generic_MSG") {
            MSG* msg = static_cast<MSG*>(message);

            if (msg->message == WM_INPUT) {
                UINT dwSize;
                static BYTE lpb[40];

                GetRawInputData((HRAWINPUT)msg->lParam, RID_INPUT,
                    lpb, &dwSize, sizeof(RAWINPUTHEADER));

                RAWINPUT* raw = (RAWINPUT*)lpb;

                if (raw->header.dwType == RIM_TYPEMOUSE) {
                    if (raw->header.hDevice == 0x0000000000000000) { //Проверяем содержится ли что-то в заголовке устройства сформировавшего raw пакет
                        EmulatingDetected = true;
                    }
                    LastMouseMessage = std::chrono::steady_clock::now();
                }
            }
            else if (msg->message == WM_LBUTTONDOWN) {
                LastMouseMessage = std::chrono::steady_clock::now(); //Смотрим когда пользователь последний раз нажимал левую кнопку мыши
            }
        }
        return false;
    }
};

// Функция для регистрации устройства Raw Input
static void RegisterRawInput(HWND hwnd) {
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01; // Generic desktop controls
    rid.usUsage = 0x02;     // Mouse
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        throw "Failed to register raw input device";
    }
}

static void ProtectQtWindow(HWND hwnd, QApplication& app)
{
    // Регистрируем Raw Input устройство
    try {
        RegisterRawInput(hwnd);
    }
    catch (const std::exception& e) {
        throw e.what();
    }

    // Устанавливаем подмену оконной процедуры
    try {
        SubclassWindow(hwnd);
    }
    catch (const std::exception& e) {
        throw e.what();
    }

    // Создаем и устанавливаем фильтр для нативных событий
    RawInputEventFilter* filter = new RawInputEventFilter();
    app.installNativeEventFilter(filter);

    // Запускаем поток для CheckCursor, для отслеживания манипуляций с курсором без отправки raw пакетов
    std::thread cursorCheckThread(CheckCursor, hwnd);
    cursorCheckThread.detach();
}