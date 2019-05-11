// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/HW/EXI/EXI_DeviceEthernetTAP_Win32.h"
#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/HW/EXI/EXI_DeviceEthernetTAP.h"

namespace Win32TAPHelper
{
bool IsTAPDevice(const TCHAR* guid)
{
  HKEY netcard_key;
  LONG status;
  DWORD len;
  int i = 0;

  status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, ADAPTER_KEY, 0, KEY_READ, &netcard_key);

  if (status != ERROR_SUCCESS)
    return false;

  for (;;)
  {
    TCHAR enum_name[256];
    TCHAR unit_string[256];
    HKEY unit_key;
    TCHAR component_id_string[] = _T("ComponentId");
    TCHAR component_id[256];
    TCHAR net_cfg_instance_id_string[] = _T("NetCfgInstanceId");
    TCHAR net_cfg_instance_id[256];
    DWORD data_type;

    len = sizeof(enum_name);
    status = RegEnumKeyEx(netcard_key, i, enum_name, &len, nullptr, nullptr, nullptr, nullptr);

    if (status == ERROR_NO_MORE_ITEMS)
      break;
    else if (status != ERROR_SUCCESS)
      return false;

    _sntprintf(unit_string, sizeof(unit_string), _T("%s\\%s"), ADAPTER_KEY, enum_name);

    status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, unit_string, 0, KEY_READ, &unit_key);

    if (status != ERROR_SUCCESS)
    {
      return false;
    }
    else
    {
      len = sizeof(component_id);
      status = RegQueryValueEx(unit_key, component_id_string, nullptr, &data_type,
                               (LPBYTE)component_id, &len);

      if (!(status != ERROR_SUCCESS || data_type != REG_SZ))
      {
        len = sizeof(net_cfg_instance_id);
        status = RegQueryValueEx(unit_key, net_cfg_instance_id_string, nullptr, &data_type,
                                 (LPBYTE)net_cfg_instance_id, &len);

        if (status == ERROR_SUCCESS && data_type == REG_SZ)
        {
          if (!_tcscmp(component_id, TAP_COMPONENT_ID) && !_tcscmp(net_cfg_instance_id, guid))
          {
            RegCloseKey(unit_key);
            RegCloseKey(netcard_key);
            return true;
          }
        }
      }
      RegCloseKey(unit_key);
    }
    ++i;
  }

  RegCloseKey(netcard_key);
  return false;
}

bool GetGUIDs(std::vector<std::basic_string<TCHAR>>& guids)
{
  LONG status;
  HKEY control_net_key;
  DWORD len;
  DWORD cSubKeys = 0;

  status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, NETWORK_CONNECTIONS_KEY, 0, KEY_READ | KEY_QUERY_VALUE,
                        &control_net_key);

  if (status != ERROR_SUCCESS)
    return false;

  status = RegQueryInfoKey(control_net_key, nullptr, nullptr, nullptr, &cSubKeys, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr);

  if (status != ERROR_SUCCESS)
    return false;

  for (DWORD i = 0; i < cSubKeys; i++)
  {
    TCHAR enum_name[256];
    TCHAR connection_string[256];
    HKEY connection_key;
    TCHAR name_data[256];
    DWORD name_type;
    const TCHAR name_string[] = _T("Name");

    len = sizeof(enum_name);
    status = RegEnumKeyEx(control_net_key, i, enum_name, &len, nullptr, nullptr, nullptr, nullptr);

    if (status != ERROR_SUCCESS)
      continue;

    _sntprintf(connection_string, sizeof(connection_string), _T("%s\\%s\\Connection"),
               NETWORK_CONNECTIONS_KEY, enum_name);

    status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, connection_string, 0, KEY_READ, &connection_key);

    if (status == ERROR_SUCCESS)
    {
      len = sizeof(name_data);
      status = RegQueryValueEx(connection_key, name_string, nullptr, &name_type, (LPBYTE)name_data,
                               &len);

      if (status != ERROR_SUCCESS || name_type != REG_SZ)
      {
        continue;
      }
      else
      {
        if (IsTAPDevice(enum_name))
        {
          guids.push_back(enum_name);
        }
      }

      RegCloseKey(connection_key);
    }
  }

  RegCloseKey(control_net_key);

  return !guids.empty();
}

bool OpenTAP(HANDLE& adapter, const std::basic_string<TCHAR>& device_guid)
{
  auto const device_path = USERMODEDEVICEDIR + device_guid + TAPSUFFIX;

  adapter = CreateFile(device_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, nullptr);

  if (adapter == INVALID_HANDLE_VALUE)
  {
    INFO_LOG(SP1, "Failed to open TAP at %s", device_path.c_str());
    return false;
  }
  return true;
}

}  // namespace Win32TAPHelper

namespace ExpansionInterface
{
CEXIEthernetTAP::~CEXIEthernetTAP()
{
  if (!IsActivated())
    return;

  // Signal read thread to exit.
  m_read_enabled.Clear();
  m_read_thread_shutdown.Set();

  // Cancel any outstanding requests from both this thread (writes), and the read thread.
  CancelIoEx(m_h_adapter, nullptr);

  // Wait for read thread to exit.
  if (m_read_thread.joinable())
    m_read_thread.join();

  // Clean-up handles
  CloseHandle(m_read_overlapped.hEvent);
  CloseHandle(m_write_overlapped.hEvent);
  CloseHandle(m_h_adapter);
  m_h_adapter = INVALID_HANDLE_VALUE;
  memset(&m_read_overlapped, 0, sizeof(m_read_overlapped));
  memset(&m_write_overlapped, 0, sizeof(m_write_overlapped));
}

bool CEXIEthernetTAP::Activate()
{
  if (IsActivated())
    return true;

  DWORD len;
  std::vector<std::basic_string<TCHAR>> device_guids;

  if (!Win32TAPHelper::GetGUIDs(device_guids))
  {
    ERROR_LOG(SP1, "Failed to find a TAP GUID");
    return false;
  }

  for (size_t i = 0; i < device_guids.size(); i++)
  {
    if (Win32TAPHelper::OpenTAP(m_h_adapter, device_guids.at(i)))
    {
      INFO_LOG(SP1, "OPENED %s", device_guids.at(i).c_str());
      break;
    }
  }
  if (m_h_adapter == INVALID_HANDLE_VALUE)
  {
    PanicAlert("Failed to open any TAP");
    return false;
  }

  /* get driver version info */
  ULONG info[3];
  if (DeviceIoControl(m_h_adapter, TAP_IOCTL_GET_VERSION, &info, sizeof(info), &info, sizeof(info),
                      &len, nullptr))
  {
    INFO_LOG(SP1, "TAP-Win32 Driver Version %d.%d %s", info[0], info[1], info[2] ? "(DEBUG)" : "");
  }
  if (!(info[0] > TAP_WIN32_MIN_MAJOR ||
        (info[0] == TAP_WIN32_MIN_MAJOR && info[1] >= TAP_WIN32_MIN_MINOR)))
  {
    PanicAlertT("ERROR: This version of Dolphin requires a TAP-Win32 driver"
                " that is at least version %d.%d -- If you recently upgraded your Dolphin"
                " distribution, a reboot is probably required at this point to get"
                " Windows to see the new driver.",
                TAP_WIN32_MIN_MAJOR, TAP_WIN32_MIN_MINOR);
    return false;
  }

  /* set driver media status to 'connected' */
  ULONG status = TRUE;
  if (!DeviceIoControl(m_h_adapter, TAP_IOCTL_SET_MEDIA_STATUS, &status, sizeof(status), &status,
                       sizeof(status), &len, nullptr))
  {
    ERROR_LOG(SP1, "WARNING: The TAP-Win32 driver rejected a"
                   "TAP_IOCTL_SET_MEDIA_STATUS DeviceIoControl call.");
    return false;
  }

  /* initialize read/write events */
  m_read_overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  m_write_overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (m_read_overlapped.hEvent == nullptr || m_write_overlapped.hEvent == nullptr)
    return false;

  m_write_buffer.reserve(1518);
  return RecvInit();
}

bool CEXIEthernetTAP::IsActivated() const
{
  return m_h_adapter != INVALID_HANDLE_VALUE;
}

void CEXIEthernetTAP::ReadThreadHandler(CEXIEthernetTAP* self)
{
  while (!self->m_read_thread_shutdown.IsSet())
  {
    DWORD transferred;

    // Read from TAP into internal buffer.
    if (ReadFile(self->m_h_adapter, &self->m_recv_buffer, BBA_RECV_SIZE, &transferred,
                 &self->m_read_overlapped))
    {
      // Returning immediately is not likely to happen, but if so, reset the event state manually.
      ResetEvent(self->m_read_overlapped.hEvent);
    }
    else
    {
      // IO should be pending.
      if (GetLastError() != ERROR_IO_PENDING)
      {
        ERROR_LOG(SP1, "ReadFile failed (err=0x%X)", GetLastError());
        continue;
      }

      // Block until the read completes.
      if (!GetOverlappedResult(self->m_h_adapter, &self->m_read_overlapped, &transferred, TRUE))
      {
        // If CancelIO was called, we should exit (the flag will be set).
        if (GetLastError() == ERROR_OPERATION_ABORTED)
          continue;

        // Something else went wrong.
        ERROR_LOG(SP1, "GetOverlappedResult failed (err=0x%X)", GetLastError());
        continue;
      }
    }

    // Copy to BBA buffer, and fire interrupt if enabled.
    if (self->m_read_enabled.IsSet())
    {
      self->m_recv_buffer_length = transferred;
      self->RecvHandlePacket();
    }
  }
}

bool CEXIEthernetTAP::SendFrame(const u8* frame, u32 size)
{
  // Check for a background write. We can't issue another one until this one has completed.
  DWORD transferred;
  if (m_write_pending)
  {
    // Wait for previous write to complete.
    if (!GetOverlappedResult(m_h_adapter, &m_write_overlapped, &transferred, TRUE))
      ERROR_LOG(SP1, "GetOverlappedResult failed (err=0x%X)", GetLastError());
  }

  // Copy to write buffer.
  m_write_buffer.assign(frame, frame + size);
  m_write_pending = true;

  // Queue async write.
  if (WriteFile(m_h_adapter, m_write_buffer.data(), size, &transferred, &m_write_overlapped))
  {
    // Returning immediately is not likely to happen, but if so, reset the event state manually.
    ResetEvent(m_write_overlapped.hEvent);
  }
  else
  {
    // IO should be pending.
    if (GetLastError() != ERROR_IO_PENDING)
    {
      ERROR_LOG(SP1, "WriteFile failed (err=0x%X)", GetLastError());
      ResetEvent(m_write_overlapped.hEvent);
      m_write_pending = false;
      return false;
    }
  }

  // Always report the packet as being sent successfully, even though it might be a lie
  SendComplete();
  return true;
}

bool CEXIEthernetTAP::RecvInit()
{
  m_read_thread = std::thread(ReadThreadHandler, this);
  return true;
}

void CEXIEthernetTAP::RecvStart()
{
  m_read_enabled.Set();
}

void CEXIEthernetTAP::RecvStop()
{
  m_read_enabled.Clear();
}
}  // namespace ExpansionInterface