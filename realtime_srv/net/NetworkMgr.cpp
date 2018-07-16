#include "realtime_srv/common/RealtimeSrvShared.h"


using namespace realtime_srv;


#ifdef IS_LINUX

namespace
{
const float		kClientDisconnectTimeout = 6.f;
}

using namespace muduo;
using namespace muduo::net;

AtomicInt32 NetworkMgr::kNewNetId;

NetworkMgr::NetworkMgr( uint16_t inPort ) :
	pktDispatcher_( inPort, PACKET_DISPATCHER_THREAD_NUM ),
	pktHandler_( pktDispatcher_.GetReceivedPacketBlockQueue(),
		std::bind( &NetworkMgr::PktProcessFunc, this, _1 ) )
{
	kNewNetId.getAndSet( 1 );
}

void NetworkMgr::DoPreparePacketToSend( ClientProxyPtr cliProxy, const uint32_t inConnFlag )
{
	shared_ptr< OutputBitStream > outputPacket( new OutputBitStream() );
	outputPacket->Write( inConnFlag );

	InFlightPacket* ifp = cliProxy->GetDeliveryNotifyManager()
		.WriteState( *outputPacket, cliProxy.get() );

	switch ( inConnFlag )
	{
		case kResetCC:
		case kWelcomeCC:
			outputPacket->Write( cliProxy->GetPlayerId() );
			outputPacket->Write( PktDispatcher::kSendPacketInterval );
			break;
		case kStateCC:
			WriteLastMoveTimestampIfDirty( *outputPacket, cliProxy );
		default:
			break;
	}
	cliProxy->GetReplicationManager().Write( *outputPacket, ifp );

	pktDispatcher_.AppendToPendingSndPktQ(
		PendingSendPacketPtr( new PendingSendPacket(
			outputPacket, cliProxy->GetUdpConnection() ) ),
		cliProxy->GetConnHoldedByThreadId() );

}

void NetworkMgr::DoProcessPkt( ReceivedPacketPtr& recvedPacket )
{
	auto it = udpConnToClientMap_.find( recvedPacket->GetUdpConn() );
	if ( it == udpConnToClientMap_.end() )
	{
		WelcomeNewClient( *recvedPacket->GetPacketBuffer(),
			recvedPacket->GetUdpConn(), recvedPacket->GetHoldedByThreadId() );
	}
	else
	{
		CheckPacketType( ( *it ).second, *recvedPacket->GetPacketBuffer() );
	}
}

void NetworkMgr::PreparePacketToSend()
{
	for ( const auto& pair : udpConnToClientMap_ )
	{
		( pair.second )->GetDeliveryNotifyManager().ProcessTimedOutPackets();

		if ( ( pair.second )->IsLastMoveTimestampDirty() )
		{
			DoPreparePacketToSend( ( pair.second ), kStateCC );
		}
	}
}

void NetworkMgr::PktProcessFunc( ReceivedPacketPtr& recvedPacket )
{
	DoProcessPkt( recvedPacket );
	worldUpdateCb_();
	PreparePacketToSend();
}

void NetworkMgr::CheckForDisconnects()
{
	float minAllowedTime =
		RealtimeSrvTiming::sInst.GetCurrentGameTime() - kClientDisconnectTimeout;

	vector< ClientProxyPtr > clientsToDisconnect;

	for ( const auto& pair : udpConnToClientMap_ )
	{
		if ( pair.second->GetLastPacketFromClientTime() < minAllowedTime )
		{ clientsToDisconnect.push_back( pair.second ); }
	}
	for ( auto cliToDC : clientsToDisconnect )
	{ cliToDC->GetUdpConnection()->forceClose(); }
}

bool NetworkMgr::Init()
{
	std::function< void() > checkDisconnCb =
		std::bind( &NetworkMgr::CheckForDisconnects, this );
	pktDispatcher_.SetInterval( std::bind(
		&PktHandler::AppendToPendingFuncs, &pktHandler_, checkDisconnCb ),
		static_cast< double >( kClientDisconnectTimeout ) );

	pktDispatcher_.SetConnCallback(
		std::bind( &NetworkMgr::OnConnOrDisconn, this, _1 ) );

	return true;
}

void NetworkMgr::OnConnOrDisconn( const UdpConnectionPtr& conn )
{
	if ( !conn->connected() )
	{
		pktHandler_.AppendToPendingFuncs(
			std::bind( &NetworkMgr::RemoveClient, this, conn ) );
	}
}

void NetworkMgr::Start()
{
	pktHandler_.Start();
	pktDispatcher_.Start();
}

void NetworkMgr::RemoveClient( const UdpConnectionPtr& conn )
{
	auto it = udpConnToClientMap_.find( conn );
	if ( it != udpConnToClientMap_.end() )
	{
		LOG( "Player %d disconnect", it->second->GetPlayerId() );
		udpConnToClientMap_.erase( conn );
	}
}

void NetworkMgr::WelcomeNewClient( InputBitStream& inInputStream,
	const UdpConnectionPtr& inUdpConnetction, const pid_t inHoldedByThreadId )
{
	uint32_t	packetType;
	inInputStream.Read( packetType );
	if ( packetType == kHelloCC
		|| packetType == kInputCC
		|| packetType == kResetedCC )
	{
		std::string playerName = "realtime_srv_test_player_name";
		//inInputStream.Read( playerName );	

		ClientProxyPtr newClientProxy =
			std::make_shared< ClientProxy >(
				this,
				playerName,
				kNewNetId.getAndAdd( 1 ),
				inHoldedByThreadId,
				inUdpConnetction );

		( udpConnToClientMap_ )[inUdpConnetction] = newClientProxy;

		GameObjPtr newGameObj = newPlayerCb_( newClientProxy );
		newGameObj->SetClientProxy( newClientProxy );
		worldRegistryCb_( newGameObj, RA_Create );

		if ( packetType == kHelloCC )
		{
			DoPreparePacketToSend( newClientProxy, kWelcomeCC );
		}
		else
		{
	 // Server reset
			newClientProxy->SetRecvingServerResetFlag( true );
			DoPreparePacketToSend( newClientProxy, kResetCC );
			LOG( "SendResetPacket" );
		}

		LOG( "a new client named '%s' as PlayerID %d ",
			newClientProxy->GetPlayerName().c_str(),
			newClientProxy->GetPlayerId() );
	}
	else
	{
		LOG_INFO << "Bad incoming packet from unknown client at socket "
			<< inUdpConnetction->peerAddress().toIpPort() << " is "
			<< " - we're under attack!!";
	}
}

void NetworkMgr::NotifyAllClient( GameObjPtr inGameObject, ReplicationAction inAction )
{
	for ( const auto& pair : udpConnToClientMap_ )
	{
		switch ( inAction )
		{
			case RA_Create:
				pair.second->GetReplicationManager().ReplicateCreate(
					inGameObject->GetObjId(), inGameObject->GetAllStateMask() );
				break;
			case RA_Destroy:
				pair.second->GetReplicationManager().ReplicateDestroy(
					inGameObject->GetObjId() );
				break;
			default:
				break;
		}
	}
}

void NetworkMgr::SetRepStateDirty( int inNetworkId, uint32_t inDirtyState )
{
	for ( const auto& pair : udpConnToClientMap_ )
	{
		pair.second->GetReplicationManager().SetReplicationStateDirty(
			inNetworkId, inDirtyState );
	}
}

#else
#include "realtime_srv/net/NetworkMgrWinS.h"
#endif


void NetworkMgr::CheckPacketType( ClientProxyPtr inClientProxy, InputBitStream& inInputStream )
{
	inClientProxy->UpdateLastPacketTime();

	uint32_t packetType = HandleServerReset( inClientProxy, inInputStream );
	switch ( packetType )
	{
		case kHelloCC:
		#ifdef IS_LINUX
			DoPreparePacketToSend( inClientProxy, kWelcomeCC );
		#else
			SendGamePacket( inClientProxy, kWelcomeCC );
		#endif //IS_LINUX
			break;
		case kInputCC:
			if ( inClientProxy->GetDeliveryNotifyManager().ReadAndProcessState( inInputStream ) )
			{
				HandleInputPacket( inClientProxy, inInputStream );
			}
			break;
		case kNullCC:
			break;
		default:
		#ifdef IS_LINUX
			LOG_INFO << "Unknown packet type received from "
				<< inClientProxy->GetUdpConnection()->peerAddress().toIpPort();
		#else
			LOG( "Unknown packet type received from %s", inClientProxy->GetSocketAddress().ToString().c_str() );
		#endif //IS_LINUX
			break;
	}
}

uint32_t NetworkMgr::HandleServerReset( ClientProxyPtr inClientProxy, InputBitStream& inInputStream )
{
	uint32_t packetType;
	inInputStream.Read( packetType );

	if ( packetType == kResetedCC )
	{
		inClientProxy->SetRecvingServerResetFlag( false );
		inInputStream.Read( packetType );
	}
	if ( inClientProxy->GetRecvingServerResetFlag() == true )
	{
	#ifdef IS_LINUX
		DoPreparePacketToSend( inClientProxy, kWelcomeCC );
	#else
		SendGamePacket( inClientProxy, kWelcomeCC );
	#endif //IS_LINUX
		return kNullCC;
	}
	else
	{
		return packetType;
	}
}

void NetworkMgr::HandleInputPacket( ClientProxyPtr inClientProxy, InputBitStream& inInputStream )
{
	uint32_t actionCount = 0;
	Action action;
	inInputStream.Read( actionCount, ACTION_COUNT_NUM );

	for ( ; actionCount > 0; --actionCount )
	{
		if ( action.Read( inInputStream ) )
		{
			if ( inClientProxy->GetUnprocessedActionList().AddMoveIfNew( action ) )
			{
				inClientProxy->SetIsLastMoveTimestampDirty( true );
			}
		}
	}
}

void NetworkMgr::WriteLastMoveTimestampIfDirty( OutputBitStream& inOutputStream,
	ClientProxyPtr inClientProxy )
{
	bool isTimestampDirty = inClientProxy->IsLastMoveTimestampDirty();
	inOutputStream.Write( isTimestampDirty );
	if ( isTimestampDirty )
	{
		inOutputStream.Write( inClientProxy->GetUnprocessedActionList()
			.GetLastMoveTimestamp() );
		inClientProxy->SetIsLastMoveTimestampDirty( false );
	}
}