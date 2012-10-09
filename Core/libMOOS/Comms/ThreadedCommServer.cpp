/*
 * ThreadedCommServer.cpp
 *
 *  Created on: Aug 29, 2011
 *      Author: pnewman
 */

#include "MOOS/libMOOS/Comms/ThreadedCommServer.h"
#include "MOOS/libMOOS/Utils/MOOSException.h"
#include "MOOS/libMOOS/Comms/XPCTcpSocket.h"
#include "MOOS/libMOOS/Utils/ConsoleColours.h"
#include "MOOS/libMOOS/Utils/ThreadPrint.h"
#include <iomanip>
#include <iterator>
#include <algorithm>

#ifndef _WIN32
    #include <sys/signal.h>
#endif

#ifdef _WIN32
#define INVALID_SOCKET_SELECT WSAEINVAL
#else
#define INVALID_SOCKET_SELECT EBADF
#endif


namespace MOOS
{

ThreadPrint gPrinter(std::cout);




ThreadedCommServer::ThreadedCommServer()
{
    // TODO Auto-generated constructor stub
	std::cerr<<MOOS::ConsoleColours::Red()<<
			"THIS IS A RISKY BUSINESS SUPPORTING ASYNCHRONOUS MOOS\n"
			<<MOOS::ConsoleColours::reset();

}

ThreadedCommServer::~ThreadedCommServer()
{
    // TODO Auto-generated destructor stub
}


/**
 * called when a new client connects. Our job is to make a new thread which will handle
 * all communications with the client. This will stop slow comms slowing everything up
 * @param pNewClient
 * @param sName
 * @return
 */
bool ThreadedCommServer::OnNewClient(XPCTcpSocket * pNewClient,char * sName)
{
    if(!BASE::OnNewClient(pNewClient,sName))
    {
        return false;
    }

    //after handshaking we will know what this client is called
    std::string sClientName = GetClientName(pNewClient);


    //here we need to start a new thread to handle our comms...
    return AddAndStartClientThread(*pNewClient,sClientName);

}

/**
 * The mechanics of starting a new thread to handle client communications
 * @param pNewClient
 * @param sName
 * @return
 */
bool ThreadedCommServer::AddAndStartClientThread(XPCTcpSocket & NewClientSocket,const std::string & sName)
{

    std::map<std::string,ClientThread*>::iterator q = m_ClientThreads.find(sName);


    if(q != m_ClientThreads.end() )
    {
        ClientThread* pExistingClientThread = q->second;

        if(pExistingClientThread)
        {
            MOOSTrace("worrying condition ::AddAndStartClientThread() killing an existing client thread");

            //stop the thread
            pExistingClientThread->Kill();

            //free up the heap (old skool - good grief)
            delete pExistingClientThread;

            //remove from map
            m_ClientThreads.erase(q);
        }
        else
        {
            return MOOSFail("logical error ::AddAndStartClientThread() NULL thread pointer");
        }
    }



    //make a new client - and show it where to put data and how to talk to a client
    bool bAsync = m_AsynchronousClientSet.find(sName)!=m_AsynchronousClientSet.end();

    ClientThread* pNewClientThread =  new  ClientThread(sName,NewClientSocket,m_SharedDataListFromClient,bAsync);

    //add to map
    m_ClientThreads[sName] = pNewClientThread;


    return pNewClientThread->Start();


}

/**
 * This is the main loop - it looks for complete Pkt being placed in the incoming list
 * and invokes a handler
 * @return true on exit
 */
bool ThreadedCommServer::ServerLoop()
{

    //eternally look at our incoming work list....
    while(1)
    {
        ClientThreadSharedData SDFromClient;

        //MOOSTrace("ThreadedCommServer::ServerLoop[%d]-  %d items to handle\n",i++,m_SharedDataListFromClient.Size());

        if(m_SharedDataListFromClient.Size()==0)
            m_SharedDataListFromClient.WaitForPush();

        m_SharedDataListFromClient.Pull(SDFromClient);

        switch(SDFromClient._Status)
        {
        case ClientThreadSharedData::PKT_READ:
        {
            ProcessClient(SDFromClient);
            break;
        }

        case ClientThreadSharedData::CONNECTION_CLOSED:
            OnClientDisconnect(SDFromClient);
            break;

        default:
            break;
        }

    }

    return true;

}



/**
 * the main handler  - a Pkt has been fetch off the work list (and is in SD)
 * we now invoke a callback and then place the return packet in the
 * same basket
 * @param SD
 * @return
 */
bool ThreadedCommServer::ProcessClient(ClientThreadSharedData &SDFromClient)
{
    bool bResult = true;

    try
    {

        //now we act on that packet
        //by way of the user supplied called back
        if(m_pfnRxCallBack!=NULL)
        {



            MOOSMSG_LIST MsgLstRx,MsgLstTx;

            //convert to list of messages
            SDFromClient._pPkt->Serialize(MsgLstRx,false);

            //is there any sort of notification going on here?
            bool bIsNotification = false;
            for(MOOSMSG_LIST::iterator q = MsgLstRx.begin();q!=MsgLstRx.end();q++)
            {
            	if(q->IsType(MOOS_NOTIFY))
            	{
            		bIsNotification= true;
            		break;
            	}
            }


            std::string sWho = SDFromClient._sClientName;

            //let owner figure out what to do !
            //this is a user supplied call back

            if(m_bQuiet)
                InhibitMOOSTraceInThisThread(false);

            if(!(*m_pfnRxCallBack)(sWho,MsgLstRx,MsgLstTx,m_pRxCallBackParam))
            {
                //client call back failed!!
                MOOSTrace(" CMOOSCommServer::ProcessClient()  pfnCallback failed\n");
            }

            if(m_bQuiet)
                InhibitMOOSTraceInThisThread(true);



            //every packet will no begin with a NULL message the double val
            //of which will be the current time on the DB's machine
            if( 1 || MsgLstTx.size()==0)
            {
                //add a default packet so client doesn't block
                CMOOSMsg NullMsg;
                NullMsg.m_dfVal = MOOSLocalTime();
                MsgLstTx.push_front(NullMsg);
            }

            //MOOSTrace("sending %d message back to client\n",MsgLstTx.size());


            //send packet back to client...
            ClientThreadSharedData SDDownStream(sWho,ClientThreadSharedData::PKT_WRITE);

            //stuff reply message into a packet
            SDDownStream._pPkt->Serialize(MsgLstTx,true);

            //first find the client object
            std::map<std::string,ClientThread*>::iterator q = m_ClientThreads.find(sWho);
            if(q == m_ClientThreads.end())
            {
                MOOSTrace("logical error - FIX ME!");
            }

            ClientThread* pClient = m_ClientThreads[sWho];

            //add it to the work load
            pClient->SendToClient(SDDownStream);

            //was there ever a notification? If not just continue
            if(bIsNotification==false)
            	return true;

            //and here if we have any new fancy asynchronous clients
            //w can send them mail as well...
            for(q=m_ClientThreads.begin();q!=m_ClientThreads.end();q++)
            {
            	ClientThread* pClient = q->second;
            	if(m_pfnFetchAllMailCallBack!=NULL && pClient->IsAsynchronous())
            	{
            		//OK this client can handle unsolicited pushes of data
            		MsgLstTx.clear();
            		if((*m_pfnFetchAllMailCallBack)(q->first,MsgLstTx,m_pFetchAllMailCallBackParam))
                    {
                    	//any pending mail?
                    	if(MsgLstTx.size()==0)
                    		continue;

                    	gPrinter.Print("sending message to "+q->first);

                    	ClientThreadSharedData SDAdditionalDownStream(sWho,
                    			ClientThreadSharedData::PKT_WRITE);

                    	//stuff all notifications into a packet
                    	SDAdditionalDownStream._pPkt->Serialize(MsgLstTx,true);

                        //add it to the work load of this client
                        pClient->SendToClient(SDAdditionalDownStream);

                    }
            	}
            }


        }
    }
    catch(CMOOSException e)
    {
        MOOSTrace("ProcessClient() Exception: %s\n", e.m_sReason);
        bResult = false;
    }

    return bResult;

}

bool ThreadedCommServer::ProcessClient()
{
	return BASE::ProcessClient();
}


bool ThreadedCommServer::OnClientDisconnect(ClientThreadSharedData &SD)
{
    //lock the base socket list
    m_SocketListLock.Lock();


    //we need to get the socket this thread is working on
    std::map<std::string,ClientThread*>::iterator q = m_ClientThreads.find(SD._sClientName);

    //we need to point the base class focus socket at this
    m_pFocusSocket = &(q->second->GetSocket());

    //now we can stop and clean up the thread
    StopAndCleanUpClientThread(SD._sClientName);


    SOCKETLIST::iterator p = std::find(m_ClientSocketList.begin(),m_ClientSocketList.end(),m_pFocusSocket);


    //now call the base class operation - this cleans down the socket from the
    //base class select
    BASE::OnClientDisconnect();

    if(p!=m_ClientSocketList.end())
        m_ClientSocketList.erase(p);

    m_SocketListLock.UnLock();

    return true;
}

bool ThreadedCommServer::OnClientDisconnect()
{
	return BASE::OnClientDisconnect();
}



bool ThreadedCommServer::StopAndCleanUpClientThread(std::string sName)
{
	MOOSTrace("ThreadedCommServer::StopAndCleanUpClientThread \n");


    //use this name to get the thread which is doing our work
    std::map<std::string,ClientThread*>::iterator q = m_ClientThreads.find(sName);

    if(q==m_ClientThreads.end())
        return MOOSFail("runtime error ThreadedCommServer::StopAndCleanUpClientThread - cannot figure out worker thread");

    //stop the thread and wait for it to return
    ClientThread* pWorker = q->second;
    pWorker->Kill();

    //remove any reference to this worker thread
    m_ClientThreads.erase(q);
    delete pWorker;
    return true;
}

bool ThreadedCommServer::TimerLoop()
{
    //we don't run absent client checks in the threaded version
    //simply quit this thread..
    return true;
}



ThreadedCommServer::ClientThread::~ClientThread()
{

}


ThreadedCommServer::ClientThread::ClientThread(const std::string & sName, XPCTcpSocket & ClientSocket,SHARED_PKT_LIST & SharedDataIncoming, bool bAsync ):
            _sClientName(sName),
            _ClientSocket(ClientSocket),
            _SharedDataIncoming(SharedDataIncoming),
			_bAsynchronous(bAsync)
{
    _Worker.Initialise(RunEntry,this);

    if(IsAsynchronous())
    {
    	_Writer.Initialise(WriteEntry,this);
    }
}



bool ThreadedCommServer::ClientThread::Run()
{


    //ignore broken pipes as is standard for network apps
#ifndef _WIN32
    signal(SIGPIPE,SIG_IGN);
#endif


    struct timeval timeout;        // The timeout value for the select system call
    fd_set fdset;                // Set of "watched" file descriptors

    while(!_Worker.IsQuitRequested())
    {

        // The socket file descriptor set is cleared and the socket file
        // descriptor contained within tcpSocket is added to the file
        // descriptor set.
        FD_ZERO(&fdset);
        FD_SET(_ClientSocket.iGetSocketFd(), &fdset);

        // The select system call is set to timeout after 1 seconds with no data existing
        // on the socket. This reinitialisation has to be here, within the loop as Linux actually writes over
        // the timeout structure on completion of select (now that was a hard bug to find)
        timeout.tv_sec    = 1;
        timeout.tv_usec = 0;



        // A select is setup to return when data is available on the socket
        // for reading.  If data is not available after 1000 useconds, select
        // returns with a value of 0.  If data is available on the socket,
        // the select returns and data can be retrieved off the socket.
        int iSelectRet = select(_ClientSocket.iGetSocketFd() + 1,
            &fdset,
            NULL,
            NULL,
            &timeout);

        // If select returns a -1, then it failed and the thread exits.
        switch(iSelectRet)
        {
        case -1:
            if(XPCSocket::iGetLastError()==INVALID_SOCKET_SELECT)
            {
                return false;
            }
            else
            {
                return false;
            }

        case 0:
            //timeout...nothing to read - spin
            break;

        default:
            //something to read (somewhere)
            if (FD_ISSET(_ClientSocket.iGetSocketFd(), &fdset) != 0)
            {
                if(!HandleClientWrite())
                {
                    //client disconnected!
                    OnClientDisconnect();
                    //MOOSTrace("socket thread for %s quits after disconnect",_sClientName.c_str());
                    return true;
                }
            }
            else
            {
                //this is strange and unexpected.....
                MOOSTrace("unexpected logical condition");
            }
            break;

        }

        //zero socket set..
        FD_ZERO(&fdset);

    }
    //MOOSTrace("socket thread for %s quits",_sClientName.c_str());
    return 0;
}

bool ThreadedCommServer::ClientThread::OnClientDisconnect()
{

    MOOSTrace("ThreadedCommServer::ClientThread::OnClientDisconnect() %s\n", _sClientName.c_str());

    //prepare to send it up the chain
    CMOOSCommPkt PktRx,PktTx;
    ClientThreadSharedData SD(_sClientName,ClientThreadSharedData::CONNECTION_CLOSED);

    //push this data back to the central thread
    _SharedDataIncoming.Push(SD);

    if(IsAsynchronous())
    {
	    _SharedDataOutgoing.Push(SD);
    }

    return true;
}


bool ThreadedCommServer::ClientThread::Kill()
{
    MOOSTrace("ThreadedCommServer::ClientThread::Kill() %s\n", _sClientName.c_str());

	if(IsAsynchronous())
	{
	    //wait for it to stop..
		if(!_Writer.Stop())
			return false;
		std::cerr<<"Writer stopped\n";

	}

    if(!_Worker.Stop())
    	return false;

    std::cerr<<"Worker stopped\n";


    return true;
}

bool ThreadedCommServer::ClientThread::Start()
{
    if(! _Worker.Start())
    	return false;

    if(IsAsynchronous())
    	if(! _Writer.Start())
    		return false;

    return true;
}


bool ThreadedCommServer::ClientThread::SendToClient(ClientThreadSharedData & OutGoing)
{
    _SharedDataOutgoing.Push(OutGoing);
    return true;
}

bool ThreadedCommServer::ClientThread::AsynchronousWriteLoop()
{

    bool bResult = true;

    try
    {
		while(!_Writer.IsQuitRequested())
		{
			ClientThreadSharedData SDDownChain;

			if(_SharedDataOutgoing.Size()==0)
			{
				_SharedDataOutgoing.WaitForPush();
			}

			_SharedDataOutgoing.Pull(SDDownChain);

			switch(SDDownChain._Status)
			{
				//we are being asked to quit
				case ClientThreadSharedData::CONNECTION_CLOSED:
				{
					std::cerr<<"Async writer "<<_sClientName<<" quits after receiving CONNECTION_CLOSED\n";
					return true;
				}

				//do normal writing
				case ClientThreadSharedData::PKT_WRITE:
				{
					if(SDDownChain._pPkt.isNull())
					{
						std::cerr<<"logical error"<< MOOSHERE;
						return false;
					}
					//send packet to client
					SendPkt(&_ClientSocket,*SDDownChain._pPkt);
					break;
				}
				default:
				{
					//we have no expectation of this signal
					MOOSTrace("logical error %s", MOOSHERE);
					return false;
				}
			}
		}
    }
    catch (CMOOSException e)
	{
	   MOOSTrace("CMOOSCommServer::ClientThread::AsynchronousWriteLoop() Exception: %s\n", e.m_sReason);
	   bResult = false;
	}

	std::cerr<<"Async writer "<<_sClientName<<" quits after thread quit requested\n";

    return bResult;
}

bool ThreadedCommServer::ClientThread::HandleClientWrite()
{
    bool bResult = true;



    try
    {
        _ClientSocket.SetReadTime(MOOSTime());


        //prepare to send it up the chain
        ClientThreadSharedData SDUpChain(_sClientName);
        SDUpChain._Status = ClientThreadSharedData::PKT_READ;

        //read input
        ReadPkt(&_ClientSocket,*SDUpChain._pPkt);



        //push this data back to the central thread
        _SharedDataIncoming.Push(SDUpChain);

        if(IsSynchronous())
        {
			//wait for data to be returned...
            ClientThreadSharedData SDDownChain(_sClientName);
			_SharedDataOutgoing.WaitForPush();
			_SharedDataOutgoing.Pull(SDDownChain);

			if(SDDownChain._Status!=ClientThreadSharedData::PKT_WRITE)
			{
				MOOSTrace("logical error %s", MOOSHERE);
				return false;
			}

			//send packet to client
			SendPkt(&_ClientSocket,*SDDownChain._pPkt);
        }

    }
    catch (CMOOSException e)
    {
        MOOSTrace("ThreadedCommServer::ClientThread::HandleClient() Exception: %s\n", e.m_sReason);
        bResult = false;
    }

    return bResult;





}




}
