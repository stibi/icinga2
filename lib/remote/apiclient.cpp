/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2014 Icinga Development Team (http://www.icinga.org)    *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "remote/apiclient.h"
#include "remote/apilistener.h"
#include "remote/apifunction.h"
#include "remote/jsonrpc.h"
#include "base/dynamictype.h"
#include "base/objectlock.h"
#include "base/utility.h"
#include "base/logger_fwd.h"
#include "base/exception.h"
#include "base/initialize.h"

using namespace icinga;

Timer::Ptr ApiClient::m_KeepAliveTimer;

INITIALIZE_ONCE(&ApiClient::StaticInitialize);

static Value SetLogPositionHandler(const MessageOrigin& origin, const Dictionary::Ptr& params);
REGISTER_APIFUNCTION(SetLogPosition, log, &SetLogPositionHandler);

ApiClient::ApiClient(const String& identity, const Stream::Ptr& stream, ConnectionRole role)
	: m_Identity(identity), m_Stream(stream), m_Role(role), m_Seen(Utility::GetTime())
{
	m_Endpoint = Endpoint::GetByName(identity);
}

void ApiClient::StaticInitialize(void)
{
	m_KeepAliveTimer = make_shared<Timer>();
	m_KeepAliveTimer->OnTimerExpired.connect(boost::bind(&ApiClient::KeepAliveTimerHandler));
	m_KeepAliveTimer->SetInterval(5);
	m_KeepAliveTimer->Start();
}

void ApiClient::Start(void)
{
	boost::thread thread(boost::bind(&ApiClient::MessageThreadProc, static_cast<ApiClient::Ptr>(GetSelf())));
	thread.detach();
}

String ApiClient::GetIdentity(void) const
{
	return m_Identity;
}

Endpoint::Ptr ApiClient::GetEndpoint(void) const
{
	return m_Endpoint;
}

Stream::Ptr ApiClient::GetStream(void) const
{
	return m_Stream;
}

ConnectionRole ApiClient::GetRole(void) const
{
	return m_Role;
}

void ApiClient::SendMessage(const Dictionary::Ptr& message)
{
	try {
		ObjectLock olock(m_Stream);
		JsonRpc::SendMessage(m_Stream, message);
		if (message->Get("method") != "log::SetLogPosition")
			m_Seen = Utility::GetTime();
	} catch (const std::exception& ex) {
		std::ostringstream msgbuf;
		msgbuf << "Error while sending JSON-RPC message for identity '" << m_Identity << "': " << DiagnosticInformation(ex);
		Log(LogWarning, "remote", msgbuf.str());

		Disconnect();
	}
}

void ApiClient::Disconnect(void)
{
	Log(LogWarning, "remote", "API client disconnected for identity '" + m_Identity + "'");
	m_Stream->Close();

	if (m_Endpoint)
		m_Endpoint->RemoveClient(GetSelf());
	else {
		ApiListener::Ptr listener = ApiListener::GetInstance();
		listener->RemoveAnonymousClient(GetSelf());
	}
}

bool ApiClient::ProcessMessage(void)
{
	Dictionary::Ptr message = JsonRpc::ReadMessage(m_Stream);

	if (!message)
		return false;

	if (message->Get("method") != "log::SetLogPosition")
		m_Seen = Utility::GetTime();

	if (m_Endpoint && message->Contains("ts")) {
		double ts = message->Get("ts");

		/* ignore old messages */
		if (ts < m_Endpoint->GetRemoteLogPosition())
			return true;

		m_Endpoint->SetRemoteLogPosition(ts);
	}

	MessageOrigin origin;
	origin.FromClient = GetSelf();

	if (m_Endpoint) {
		if (m_Endpoint->GetZone() != Zone::GetLocalZone())
			origin.FromZone = m_Endpoint->GetZone();
		else
			origin.FromZone = Zone::GetByName(message->Get("originZone"));
	}

	String method = message->Get("method");

	Log(LogDebug, "remote", "Received '" + method + "' message from '" + m_Identity + "'");

	Dictionary::Ptr resultMessage = make_shared<Dictionary>();

	try {
		ApiFunction::Ptr afunc = ApiFunction::GetByName(method);

		if (!afunc)
			BOOST_THROW_EXCEPTION(std::invalid_argument("Function '" + method + "' does not exist."));

		resultMessage->Set("result", afunc->Invoke(origin, message->Get("params")));
	} catch (std::exception& ex) {
		resultMessage->Set("error", DiagnosticInformation(ex));
	}

	if (message->Contains("id")) {
		resultMessage->Set("jsonrpc", "2.0");
		resultMessage->Set("id", message->Get("id"));
		JsonRpc::SendMessage(m_Stream, resultMessage);
	}

	return true;
}

void ApiClient::MessageThreadProc(void)
{
	Utility::SetThreadName("API Client");

	try {
		while (ProcessMessage())
			; /* empty loop body */

		Disconnect();
	} catch (const std::exception& ex) {
		Log(LogWarning, "remote", "Error while reading JSON-RPC message for identity '" + m_Identity + "': " + DiagnosticInformation(ex));
	}
}

void ApiClient::KeepAliveTimerHandler(void)
{
	double now = Utility::GetTime();

	BOOST_FOREACH(const Endpoint::Ptr& endpoint, DynamicType::GetObjects<Endpoint>()) {
		if (endpoint->GetZone() == Zone::GetLocalZone())
			continue;

		if (endpoint->GetSyncing() || endpoint->GetKeepAlive() <= 0)
			continue;

		double timeout = now - endpoint->GetKeepAlive();

		BOOST_FOREACH(const ApiClient::Ptr& client, endpoint->GetClients()) {
			if (client->m_Seen < timeout) {
				Log(LogNotice, "remote", "Closing connection with inactive endpoint '" + endpoint->GetName() + "'");
				client->Disconnect();
			}
		}
	}


	ApiListener::Ptr listener = ApiListener::GetInstance();

	if (listener) {
		double timeout = now - 60;

		BOOST_FOREACH(const ApiClient::Ptr& client, listener->GetAnonymousClients()) {
			if (client->m_Seen < timeout) {
				Log(LogNotice, "remote", "Closing connection with inactive anonymous endpoint '" + client->GetIdentity() + "'");
				client->Disconnect();
			}
		}
	}

}

Value SetLogPositionHandler(const MessageOrigin& origin, const Dictionary::Ptr& params)
{
	if (!params)
		return Empty;

	double log_position = params->Get("log_position");
	Endpoint::Ptr endpoint = origin.FromClient->GetEndpoint();

	if (!endpoint)
		return Empty;

	if (log_position > endpoint->GetLocalLogPosition())
		endpoint->SetLocalLogPosition(log_position);

	return Empty;
}
