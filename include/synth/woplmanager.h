//
// woplmanager.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// Scans SD and USB root directories for *.wopl OPL3 bank files.
//

#ifndef _woplmanager_h
#define _woplmanager_h

#include <circle/string.h>
#include <cstddef>

class CWoplBankManager
{
public:
	CWoplBankManager();
	~CWoplBankManager() = default;

	bool ScanBanks();

	size_t GetBankCount() const { return m_nBanks; }
	const char* GetBankPath(size_t nIndex) const;
	const char* GetBankName(size_t nIndex) const;

	static constexpr size_t MaxBanks = 64;

private:
	struct TBankEntry
	{
		CString Name;
		CString Path;
	};

	void CheckBank(const char* pFullPath, const char* pFileName);
	static bool IsSupportedExtension(const char* pFileName);

	size_t m_nBanks;
	TBankEntry m_BankList[MaxBanks];

	inline static bool BankListComparator(const TBankEntry& lhs, const TBankEntry& rhs);
};

#endif
