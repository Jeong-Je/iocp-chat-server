#include "stdafx.h"
#include <winsock2.h>
#pragma comment(lib, "ws2_32")
#include <windows.h>
#include <list>

typedef struct _USSERSESSION
{
	SOCKET hSocket;
	char buffer[8192];	//8KB
} USERSESSION;

#define MAX_THREAD_CNT 4

CRITICAL_SECTION g_cs;
std::list<SOCKET> g_listClient; //연결된 클라이언트 소켓 리스트
SOCKET g_hSocket;				//서버의 리슨 소켓
HANDLE g_hIocp;					//IOCP 핸들

//연결된 모든 클라이언트한테 메세지를 전송한다.
void sendMessageAll(char* pszMessage, int nSize)
{
	std::list<SOCKET>::iterator it;

	//중간에 다른 클라이언트의 채팅이 들어 오거나
	//소켓이 끊겨서 리스트의 변형이 생기는 경우 리스트 충돌이 발생할 수 있으므로 
	//임계영역을 설정한다. 
	::EnterCriticalSection(&g_cs);	//임계영역 시작
	for (it = g_listClient.begin(); it != g_listClient.end(); it++)
		::send(*it, pszMessage, nSize, 0);
	::LeaveCriticalSection(&g_cs);	//임계영역 끝
}

//클라이언트 및 리슨 소켓 닫기
void CloseClient(SOCKET hSock)
{
	::shutdown(hSock, SD_BOTH);
	::closesocket(hSock);

	::EnterCriticalSection(&g_cs);
	g_listClient.remove(hSock);
	::LeaveCriticalSection(&g_cs);
}


DWORD WINAPI ThreadComplete(LPVOID pParam)
{
	DWORD			dwTransferredSize = 0;
	DWORD			dwFlag = 0;
	USERSESSION*	pSession = NULL;
	LPWSAOVERLAPPED pWol = NULL;
	BOOL			bResult;

	puts("[IOCP 작업자 스레드 시작]");
	while (true) 
	{
		bResult = ::GetQueuedCompletionStatus(
			g_hIocp,				//Dequeue할 IOCP 핸들
			&dwTransferredSize,		//수신한 데이터 크기
			(PULONG_PTR)&pSession,	//수신된 데이터가 저장된 메모리
			&pWol,					//OVERLAPPED 구조체
			INFINITE);				//이벤트를 무한정 대기

		//IOCP Queue에서 꺼내올 클라이언트 소켓이 있는 경우 
		if (bResult == TRUE)
		{
			//정상적인 경우

			//1. 클라이언트가 소켓을 정상적으로 닫고 연결을 끊은 경우
			if (dwTransferredSize == 0)
			{
				CloseClient(pSession->hSocket);
				delete pWol;
				delete pSession;
				puts("\tGQCS: 클라이언트가 정상적으로 연결을 종료함.");
			}

			//2. 클라이언트가 보낸 데이터를 수신한 경우
			else
			{
				printf("[채팅 로그]%s\n", pSession->buffer);
				sendMessageAll(pSession->buffer, dwTransferredSize);
				memset(pSession->buffer, 0, sizeof(pSession->buffer));

				//다시 IOCP에 등록
				DWORD dwReceiveSize = 0;
				DWORD dwFlag = 0;
				WSABUF wsaBuf = { 0 };
				wsaBuf.buf = pSession->buffer;
				wsaBuf.len = sizeof(pSession->buffer);

				::WSARecv(
					pSession->hSocket,	//클라이언트 소켓 핸들
					&wsaBuf,			//WSABUF 구조체 배열의 주소
					1,					//배열 요소의 개수
					&dwReceiveSize,
					&dwFlag,
					pWol,
					NULL);
				if (::WSAGetLastError() != WSA_IO_PENDING)
					puts("\tGQCS: ERROR: WSARecv()");
			}
		}
		else
		{
			//비정상적인 경우

			//3. 완료 큐에서 완료 패킷을 꺼내지 못하고 반환한 경우.
			if (pWol == NULL)
			{
				//IOCP 핸들이 닫힌 경우(서버를 종료하는 경우)도 해당된다.
				puts("\tGQCS: IOCP 핸들이 닫혔습니다.");
				break;
			}


			//4. 클라이언트가 비정상적으로 종료됐거나
			//   서버가 먼저 연결을 종료한 경우.
			else
			{
				if (pSession != NULL)
				{
					CloseClient(pSession->hSocket);
					delete pWol;
					delete pSession;
				}
				puts("\tGQCS: 서버 종료 혹은 클라이언트 비정상적 연결 종료");
			}
		}

	}

	puts("[IOCP 작업자 스레드 종료]");
	return 0;
}


DWORD WINAPI ThreadAcceptLoop(LPVOID pParam)
{
	LPWSAOVERLAPPED	pWol = NULL;
	DWORD			dwReceiveSize, dwFlag;
	USERSESSION*	pNewUser;
	int				nAddrSize = sizeof(SOCKADDR);
	WSABUF			wsaBuf;
	SOCKADDR		ClientAddr;
	SOCKET			hClient;
	int				nRecvResult = 0;

	while ((hClient = ::accept(g_hSocket, &ClientAddr, &nAddrSize)) != INVALID_SOCKET)
	{
		puts("새 클라이언트가 연결되었습니다.");
		::EnterCriticalSection(&g_cs);
		g_listClient.push_back(hClient);
		::LeaveCriticalSection(&g_cs);

		//새 클라이언트에 대한 세션 객체 생성
		pNewUser = new USERSESSION;
		::ZeroMemory(pNewUser, sizeof(USERSESSION));
		pNewUser->hSocket = hClient;

		//비동기 수신 처리를 위한 OVERLAPPED 구조체 생성.
		pWol = new WSAOVERLAPPED;
		::ZeroMemory(pWol, sizeof(WSAOVERLAPPED));

		//(연결된) 클라이언트 소켓 핸들을 IOCP에 연결.
		//메인 함수 다음으로 두 번째로 등장하는 CreateIoCompletionPort 함수
		//두 번째로 호출될 때는 첫 번째와 기능이 다르다.
		//지금은 클라이언트 소켓을 감시해달라고 IOCP Queue에 가져다 붙이는 기능 
		::CreateIoCompletionPort(
			(HANDLE)hClient,
			g_hIocp,
			(ULONG_PTR)pNewUser, // KEY (식별 가능한 값이면 가능)
			0
		);

		dwReceiveSize = 0;
		dwFlag = 0;
		wsaBuf.buf = pNewUser->buffer;
		wsaBuf.len = sizeof(pNewUser->buffer);

		//클라이언트가 보낸 정보를 비동기 수신한다.
		nRecvResult = ::WSARecv(hClient, &wsaBuf, 1, &dwReceiveSize, &dwFlag, pWol, NULL);
		if (::WSAGetLastError() != WSA_IO_PENDING)
		{
			puts("ERROR: WSARecv() != WSA_IO_PENDING");
		}

	}
	return 0;
}


int _tmain(int argc, _TCHAR* argv[])
{
	//윈속 초기화
	WSADATA wsa = { 0 };
	if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) 
	{
		puts("ERROR: 윈속을 초기화 할 수 없습니다.");
		return 0;
	}

	//임계 영역 객체 생성
	::InitializeCriticalSection(&g_cs);

	//IOCP 생성
	//처음 CreateIoCompletionPort를 호출하면 커널에 IOCP Queue가 생성된다.
	g_hIocp = ::CreateIoCompletionPort(
		INVALID_HANDLE_VALUE,
		NULL,
		0,
		0);

	if (g_hIocp == NULL) 
	{
		puts("ERROR: IOCP를 생성할 수 없습니다.");
		return 0;
	}

	//IOCP 스레드를 생성
	//입·출력을 담당할 스레드를 미리 생성해둔다.
	//(식당에 손님이 들어오면 그때 종업원을 고용하나? → ❌ 미리 고용해둬야지)
	//스레드를 몇 개 생성해야되는데? → 고민해 볼 문제 (코어의 수랑 똑같을 수 있고 배수가 될 수도 있고, 성능 테스트가 필요)
	HANDLE hThread;
	DWORD dwThreadID;
	for (int i = 0; i < MAX_THREAD_CNT; i++)
	{
		dwThreadID = 0;
		//클라이언트로부터 문자열을 수신함.
		hThread = ::CreateThread(NULL, //보안속성 상속
			0,						   //스택 메모리는 기본크기(1MB)
			ThreadComplete,			   //스레드로 실행할 함수이름
			(LPVOID)NULL,              //
			0,                         //생성 플래그는 기본값 사용
			&dwThreadID);              //생성된 스레드ID가 저장될 변수 주소

		::CloseHandle(hThread);
	}


	//서버 리슨 소켓 생성
	g_hSocket = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

	//bind()/listen()
	SOCKADDR_IN addrsvr;
	addrsvr.sin_family = AF_INET;
	addrsvr.sin_addr.S_un.S_addr = ::htonl(INADDR_ANY);
	addrsvr.sin_port = ::htons(25000);

	if (::bind(g_hSocket, (SOCKADDR*)&addrsvr, sizeof(SOCKADDR_IN)) == SOCKET_ERROR)
	{
		puts("ERROR: 포트가 이미 사용 중입니다.");
		return 0;
	}

	if (::listen(g_hSocket, SOMAXCONN) == SOCKET_ERROR)
	{
		puts("ERROR: 리슨 상태로 전환할 수 없습니다.");
		return 0;
	}

	//반복해서 클라이언트의 연결을 accept()한다.
	hThread = ::CreateThread(NULL, 0, ThreadAcceptLoop, (LPVOID)NULL, 0, &dwThreadID);
	::CloseHandle(hThread);

	//_tmain() 함수가 반환하지 않도록 대기한다.
	puts("*** IOCP 채팅 서버를 시작합니다! ***");
	while (true)
		getchar();

	return 0;
}