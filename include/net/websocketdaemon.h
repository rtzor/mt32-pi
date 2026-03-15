//
// websocketdaemon.h
//
// WebSocket server for mt32-pi: pushes sequencer status to browser clients
// and accepts MIDI note commands.
//

#ifndef _net_websocketdaemon_h
#define _net_websocketdaemon_h

#include <circle/sched/task.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/types.h>

class CMT32Pi;

class CWebSocketDaemon : public CTask
{
public:
	CWebSocketDaemon(CNetSubSystem* pNetSubSystem, CMT32Pi* pMT32Pi, u16 nPort = 8765);
	virtual ~CWebSocketDaemon() override;

	void Run() override;

private:
	CNetSubSystem* m_pNetSubSystem;
	CMT32Pi*       m_pMT32Pi;
	u16            m_nPort;
};

#endif
