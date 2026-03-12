//
// webdaemon.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _webdaemon_h
#define _webdaemon_h

#include <circle/net/httpdaemon.h>

class CMT32Pi;

class CWebDaemon : protected CHTTPDaemon
{
public:
	CWebDaemon(CNetSubSystem* pNetSubSystem, CMT32Pi* pMT32Pi, u16 nPort = 80);
	virtual ~CWebDaemon() override;

	CHTTPDaemon* CreateWorker(CNetSubSystem* pNetSubSystem, CSocket* pSocket) override;
	THTTPStatus GetContent(const char* pPath,
		const char* pParams,
		const char* pFormData,
		u8* pBuffer,
		unsigned* pLength,
		const char** ppContentType) override;

private:
	CWebDaemon(CNetSubSystem* pNetSubSystem, CMT32Pi* pMT32Pi, CSocket* pSocket, u16 nPort);

	CMT32Pi* m_pMT32Pi;
	u16 m_nPort;
};

#endif
