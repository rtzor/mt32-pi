//
// woplmanager.cpp
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// Scans SD and USB root directories for *.wopl OPL3 bank files.
//

#include <circle/logger.h>
#include <fatfs/ff.h>

#include "synth/woplmanager.h"
#include "utility.h"

LOGMODULE("woplmanager");

static const char* const Disks[] = { "SD", "USB" };

CWoplBankManager::CWoplBankManager()
	: m_nBanks(0)
{
}

bool CWoplBankManager::ScanBanks()
{
	// Clear existing entries
	for (size_t i = 0; i < m_nBanks; ++i)
		m_BankList[i] = TBankEntry();

	m_nBanks = 0;

	DIR Dir;
	FILINFO FileInfo;
	FRESULT Result;
	CString DirPath;

	for (auto pDisk : Disks)
	{
		// Scan the root directory of each disk
		DirPath.Format("%s:", pDisk);
		Result = f_findfirst(&Dir, &FileInfo, DirPath, "*");

		while (Result == FR_OK && *FileInfo.fname && m_nBanks < MaxBanks)
		{
			if (!(FileInfo.fattrib & (AM_DIR | AM_HID | AM_SYS)))
			{
				if (IsSupportedExtension(FileInfo.fname))
				{
					CString FullPath;
					FullPath.Format("%s:/%s", pDisk, FileInfo.fname);
					CheckBank(FullPath, FileInfo.fname);
				}
			}
			Result = f_findnext(&Dir, &FileInfo);
		}
	}

	if (m_nBanks > 0)
	{
		Utility::QSort(m_BankList, BankListComparator, 0, m_nBanks - 1);

		LOGNOTE("%d WOPL bank(s) found:", m_nBanks);
		for (size_t i = 0; i < m_nBanks; ++i)
			LOGNOTE("  %d: %s", i, static_cast<const char*>(m_BankList[i].Path));

		return true;
	}

	LOGNOTE("No WOPL banks found on SD or USB");
	return false;
}

const char* CWoplBankManager::GetBankPath(size_t nIndex) const
{
	return nIndex < m_nBanks ? static_cast<const char*>(m_BankList[nIndex].Path) : nullptr;
}

const char* CWoplBankManager::GetBankName(size_t nIndex) const
{
	if (nIndex >= m_nBanks)
		return nullptr;
	if (m_BankList[nIndex].Name.GetLength() == 0)
		return static_cast<const char*>(m_BankList[nIndex].Path);
	return static_cast<const char*>(m_BankList[nIndex].Name);
}

void CWoplBankManager::CheckBank(const char* pFullPath, const char* pFileName)
{
	if (m_nBanks >= MaxBanks)
		return;

	m_BankList[m_nBanks].Path = pFullPath;
	m_BankList[m_nBanks].Name = pFileName;
	++m_nBanks;
}

bool CWoplBankManager::IsSupportedExtension(const char* pFileName)
{
	// Find the last '.' in the filename
	const char* pDot = nullptr;
	for (const char* p = pFileName; *p; ++p)
		if (*p == '.') pDot = p;

	if (!pDot)
		return false;

	// Case-insensitive compare of extension
	const char* ext = pDot + 1;
	// ".wopl"
	if ((ext[0] == 'w' || ext[0] == 'W') &&
	    (ext[1] == 'o' || ext[1] == 'O') &&
	    (ext[2] == 'p' || ext[2] == 'P') &&
	    (ext[3] == 'l' || ext[3] == 'L') &&
	     ext[4] == '\0')
		return true;

	// ".op2"
	if ((ext[0] == 'o' || ext[0] == 'O') &&
	    (ext[1] == 'p' || ext[1] == 'P') &&
	     ext[2] == '2' &&
	     ext[3] == '\0')
		return true;

	return false;
}

inline bool CWoplBankManager::BankListComparator(const TBankEntry& lhs, const TBankEntry& rhs)
{
	// Lexicographic sort by name
	const char* a = static_cast<const char*>(lhs.Name);
	const char* b = static_cast<const char*>(rhs.Name);
	while (*a && *b)
	{
		char ca = (*a >= 'A' && *a <= 'Z') ? (*a - 'A' + 'a') : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (*b - 'A' + 'a') : *b;
		if (ca < cb) return true;
		if (ca > cb) return false;
		++a; ++b;
	}
	return *a == '\0' && *b != '\0';
}
