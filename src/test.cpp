#include "FasttunBase.h"
#include "Listener.h"
#include "Connection.h"
#include "KcpTunnel.h"
#include "FastConnection.h"
#include "Cache.h"

using namespace tun;

//--------------------------------------------------------------------------
class ClientBridge : public Connection::Handler
{
  public:
	struct Handler
	{
		virtual void onIntConnDisconnected(ClientBridge *pBridge) = 0;
		virtual void onIntConnError(ClientBridge *pBridge) = 0;
	};
	
    ClientBridge(EventPoller *poller, Handler *l)
			:mEventPoller(poller)
			,mpHandler(l)
			,mpIntConn(NULL)
			,mpExtConn(NULL)
			,mCache(NULL)
			,mLastExtConnTime(0)
	{
		mCache = new MyCache(this);
	}
	
    virtual ~ClientBridge()
	{		
		shutdown();
		delete mCache;
	}

	bool acceptConnection(int connfd)
	{
		assert(NULL == mpIntConn && NULL == mpExtConn &&
			   "NULL == mpIntConn && NULL == mpExtConn");

		mpIntConn = new Connection(mEventPoller);
		if (!mpIntConn->acceptConnection(connfd))
		{
			delete mpIntConn;
			mpIntConn = NULL;
			return false;
		}
		mpIntConn->setEventHandler(this);

		mpExtConn = new Connection(mEventPoller);
		mLastExtConnTime = getTickCount();
		if (!mpExtConn->connect("127.0.0.1", 5082))
		{
			mpIntConn->shutdown();
			delete mpIntConn;
			mpIntConn = NULL;
			delete mpExtConn;
			mpExtConn = NULL;
			return false;
		}
		mpExtConn->setEventHandler(this);
		
		return true;
	}

	void shutdown()
	{
		if (mpIntConn)
		{
			mpIntConn->shutdown();
			delete mpIntConn;
			mpIntConn = NULL;
		}
		if (mpExtConn)
		{
			mpExtConn->shutdown();
			delete mpExtConn;
			mpExtConn = NULL;
		}
	}	

	Connection* getIntConn() const
	{
		return mpIntConn;
	}

	// Connection::Handler
	virtual void onConnected(Connection *pConn)
	{
		if (pConn == mpExtConn)
		{
			logTraceLn("ss connected!");
			_flushAll();
		}
	}
	
	virtual void onDisconnected(Connection *pConn)
	{
		if (pConn == mpIntConn)
		{
			if (mpHandler)
				mpHandler->onIntConnDisconnected(this);
		}
	}

	virtual void onRecv(Connection *pConn, const void *data, size_t datalen)
	{
		if (pConn == mpIntConn)
		{
			logTraceLn("clientint onRecv len="<<datalen);
			if (!mpExtConn->isConnected())
			{
				_reconnectExternal();
				mCache->cache(data, datalen);
			}
			else
			{
				_flushAll();
				mpExtConn->send(data, datalen);
				logTraceLn("mpExtConn->send len="<<datalen);
			}
		}
		else
		{
			logTraceLn("clientext onRecv len="<<datalen);
			mpIntConn->send(data, datalen);
		}
	}
	
	virtual void onError(Connection *pConn)
	{
		if (pConn == mpIntConn)
		{
			if (mpHandler)
				mpHandler->onIntConnError(this);
		}
	}		

	void _reconnectExternal()
	{
		ulong curtick = getTickCount();
		if (curtick > mLastExtConnTime+1000)
		{
			mLastExtConnTime = curtick;
			mpExtConn->connect("127.0.0.1", 5082);
		}		
	}

	void _flushAll()
	{
		if (mpExtConn->isConnected())
		{
			mCache->flushAll();
		}
	}
	void flush(const char *data, size_t datalen)
	{
		mpExtConn->send(data, datalen);
	}
	
  private:	
	typedef Cache<ClientBridge> MyCache;
	
	EventPoller *mEventPoller;
	Handler *mpHandler;
	
	Connection *mpIntConn;
	Connection *mpExtConn;

	MyCache *mCache;

	ulong mLastExtConnTime;
};
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
class Client : public Listener::Handler, public ClientBridge::Handler
{
  public:
    Client(EventPoller *poller)
			:Listener::Handler()
			,mEventPoller(poller)
			,mListener(poller)
			,mBridges()
	{
	}
	
    virtual ~Client()
	{
	}

	bool create()
	{
		if (!mListener.create("127.0.0.1", 5081))
		{
			logErrorLn("create listener failed.");
			return false;
		}
		mListener.setEventHandler(this);

		return true;
	}

	void finalise()
	{		
		mListener.finalise();
		BridgeList::iterator it = mBridges.begin();
		for (; it != mBridges.end(); ++it)
		{
			ClientBridge *bridge = *it;
			if (bridge)
			{
				bridge->shutdown();
				delete bridge;
			}
		}
		mBridges.clear();
	}

	virtual void onAccept(int connfd)
	{
		ClientBridge *bridge = new ClientBridge(mEventPoller, this);
		if (!bridge->acceptConnection(connfd))
		{
			delete bridge;
			return;
		}

		mBridges.insert(bridge);
	}

	virtual void onIntConnDisconnected(ClientBridge *pBridge)
	{				
		onBridgeShut(pBridge);
	}
	
	virtual void onIntConnError(ClientBridge *pBridge)
	{		
		onBridgeShut(pBridge);
	}	   	
	
  private:
	void onBridgeShut(ClientBridge *pBridge)
	{		
		BridgeList::iterator it = mBridges.find(pBridge);
		if (it != mBridges.end())
		{			
			mBridges.erase(it);
		}

		pBridge->shutdown();
		delete pBridge;
	}

	char* getPeerAddrInfo(ClientBridge *pBridge, char *info, int len)
	{
		char peerIp[MAX_BUF] = {0};
		if (pBridge->getIntConn() && pBridge->getIntConn()->getPeerIp(peerIp, MAX_BUF))
		{
			snprintf(info, len, "%s:%d", peerIp, pBridge->getIntConn()->getPeerPort());
			return info;
		}
		return NULL;
	}	
  private:
	typedef std::set<ClientBridge *> BridgeList;
	
	EventPoller *mEventPoller;
	Listener mListener;

	BridgeList mBridges;
};
//--------------------------------------------------------------------------

int main(int argc, char *argv[])
{
	// initialise tracer
	core::createTrace();
	core::output2Console();
	core::output2Html("fasttun_client.html");

	// create event poller
	EventPoller *netPoller = EventPoller::create();

	// create client
	Client cli(netPoller);
	if (!cli.create())
	{
		logErrorLn("create client error!");
		delete netPoller;
		core::closeTrace();
		exit(1);
	}	

	for (;;)
	{
		netPoller->processPendingEvents(0.016);
	}	

	cli.finalise();	
	delete netPoller;

	// close tracer
	core::closeTrace();
	exit(0);
}