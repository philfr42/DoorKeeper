/*
 * Copyright (C) 2017 A. Koller - akandroid75@gmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <DoorKeeper.h>
#include <Esp.h>
#include <ESP8266mDNS.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiType.h>
#include <HardwareSerial.h>
#include <include/wl_definitions.h>
#include <IPAddress.h>
#include <pins_arduino.h>
#include <stddef.h>
#include <SNTPClock.h>
#include <SNTPTime.h>
#include <Vars.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WString.h>
#include <cstring>

extern "C" {
#include <sntp.h>
}

const arducryptkeypair ServerKey =
		{
		// pubkey
				{ 0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b,
						0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72,
						0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68,
						0xf7, 0x07, 0x51, 0x1a },
				// privkey
				{ 0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84,
						0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5,
						0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03,
						0x1c, 0xae, 0x7f, 0x60 } };

const User testuser =
		{
		// pubkey
				{
				0xa5,0x6a,0x9a,0xdb,0x8b,0x00,0x9e,0x47,
				0xac,0xc9,0x13,0xc5,0x1a,0xdb,0xc3,0x3e,
				0x24,0x44,0xbc,0xb0,0x59,0x04,0x79,0xb4,
				0x0e,0x02,0xd2,0x44,0xc7,0xce,0xc1,0xac
				},
				// valid from
				0xff, 0xff, 0xff,
				// valid to
				0xee, 0xee, 0xee };

//#define SERVERPORT 23

DoorKeeperConfig dkconfig;

//Externals in SNTPClock.cpp
extern SNTPClock Clock;

const int MAX_SRV_CLIENTS = 3;
WiFiServer server(23);
WiFiClient serverClients[MAX_SRV_CLIENTS];
DoorKeeperSession sessions[MAX_SRV_CLIENTS];

DoorKeeper keeper;

//CallBackFunction
void ClockCbFunction() {

	Clock.attachCb((ulong) 1, (SNTPClock::callback_t) ClockCbFunction);
	keeper.CB1000ms(Clock.getTimeSeconds());
	// to stop it detach the function
	//Clock.detachCb();
}

boolean static defaultHandler(uint8_t messagetype, uint8_t reservedByte, MessagePayload* payload, DoorKeeperMessage* outbuffer) {
	DOORKEEPERDEBUG_PRINTLN(F("default handler was called!"));
	DOORKEEPERDEBUG_PRINT("messagetype: ");DOORKEEPERDEBUG_PRINT(messagetype);
	DOORKEEPERDEBUG_PRINT(" reservebyte: ");DOORKEEPERDEBUG_PRINTLN(reservedByte);
	DOORKEEPERDEBUG_PRINT("data: ");DOORKEEPERDEBUG_HEXPRINT((uint8_t*)payload,sizeof(MessagePayload));
	DOORKEEPERDEBUG_PRINTLN();
	// set return message
	payload->data.custom.data[0] = 0x66;
	// set type byte
	outbuffer->messagetype = 0xaa;
	// return true to send response
	return true;
}

void generateNewSignKeyPair() {
	uint8_t newPrivateKey[KEYSIZE];
	uint8_t newPublicKey[KEYSIZE];
	arducrypt::generateSigKeyPair(newPrivateKey, newPublicKey);
	DOORKEEPERDEBUG_PRINT("new sign key pair: ");
	DOORKEEPERDEBUG_PRINT("public key: ");
	DOORKEEPERDEBUG_HEXPRINT(newPublicKey,KEYSIZE);
	DOORKEEPERDEBUG_PRINT(", private key: ");
	DOORKEEPERDEBUG_HEXPRINT(newPrivateKey,KEYSIZE);
	DOORKEEPERDEBUG_PRINTLN();
}


void setup() {

//	wdt_disable();
	Serial.begin(115200);
	delay(2000);
	WiFi.mode(WIFI_STA);
	WiFi.disconnect();

	sntp_init();
	sntp_setservername(0, (char*) "de.pool.ntp.org");
	sntp_setservername(1, (char*) "time.windows.com");
	sntp_setservername(2, (char*) "time.nist.gov");
	sntp_set_timezone(0);

	// Connect to WiFi network
	DOORKEEPERDEBUG_PRINT(F("connecting to: "));
	DOORKEEPERDEBUG_PRINTLN(ssid);
	WiFi.begin(ssid, password);
	int retry = 0;
	while (WiFi.status() != WL_CONNECTED) {
		DOORKEEPERDEBUG_PRINT(".");
		delay(500);
		if (++retry > 20) {
			WiFi.begin(ssid, password);
			Serial.println();
			retry = 0;
		}
	}
	DOORKEEPERDEBUG_PRINT(F("\nconnected to: "));
	DOORKEEPERDEBUG_PRINTLN(WiFi.SSID());
	DOORKEEPERDEBUG_PRINT(F("signal "));
	DOORKEEPERDEBUG_PRINT(WiFi.RSSI());
	DOORKEEPERDEBUG_PRINTLN(F(" dBm"));
	DOORKEEPERDEBUG_PRINT(F("IP address: "));
	DOORKEEPERDEBUG_PRINTLN(WiFi.localIP());

	delay(1000);
	// true: we see a timestamp once an hour (GMT)
	Serial.setDebugOutput(false);
	Clock.begin("de.pool.ntp.org", 3600, 1);
	delay(1000);
	ClockCbFunction();

	if (MDNS.begin("doorkeeper")) {
		DOORKEEPERDEBUG_PRINTLN("MDNS responder started");
	}

	server.begin();
	server.setNoDelay(true);

	// config
	dkconfig.serverkeys = (arducryptkeypair*)&ServerKey;

	dkconfig.pins[0].portpin = D6;
	dkconfig.pins[0].initstate = HIGH;
	dkconfig.pins[0].OFF = HIGH;
	dkconfig.pins[0].ON = LOW;

	dkconfig.pins[1].portpin = D7;
	dkconfig.pins[1].initstate = HIGH;
	dkconfig.pins[1].OFF = HIGH;
	dkconfig.pins[1].ON = LOW;

	dkconfig.pins[2].portpin = D2;
	dkconfig.pins[2].initstate = LOW;
	dkconfig.pins[2].OFF = LOW;
	dkconfig.pins[2].ON = HIGH;

	dkconfig.pins[3].portpin = D1;
	dkconfig.pins[3].initstate = LOW;
	dkconfig.pins[3].OFF = LOW;
	dkconfig.pins[3].ON = HIGH;


	keeper.initKeeper(&dkconfig);

	// add test user from config
	keeper.addUser((User*)&testuser);
	// add callback
	keeper.addDefaultHandler(&defaultHandler);
	// this is really hacky! :(
	// we should also use DCF77 for time information
	keeper.initTime((timestruct*) Clock.getTimeStruct());

	DOORKEEPERDEBUG_PRINT("DoorKeeperMessageSize: ");
	DOORKEEPERDEBUG_PRINTLN(DoorKeeperMessageSize);
	DOORKEEPERDEBUG_PRINT("sizeof(MessageData): ");
	DOORKEEPERDEBUG_PRINTLN(sizeof(MessageData));
//	generateNewSignKeyPair();
}

extern const uint32_t DoorKeeperMessageSize;

void copyToArray(String* s, char* ar) {
	int len = s->length() + 1;
	s->toCharArray(ar, len, 0);
}

DoorKeeperSession* getSession(String sessionid) {
	char tmp[sessionid.length()];
	DOORKEEPERDEBUG_PRINTLN(sessionid.length());
	//sessionid.toCharArray(tmp, sessionid.length(), 0);
	copyToArray(&sessionid, tmp);
	DOORKEEPERDEBUG_PRINT("getSession: ");
	DOORKEEPERDEBUG_PRINTLN(tmp);
	for (int i = 0; i < MAX_SRV_CLIENTS; i++) {
		if (memcmp(sessions[i].name, tmp, sessionid.length()) == 0) {
			return &sessions[i];
		}
	}
	return NULL;
}

DoorKeeperSession* createSession(String sessionid) {
	char tmp[sessionid.length()];
	//sessionid.toCharArray(tmp, sessionid.length(), 0);
	copyToArray(&sessionid, tmp);
	DOORKEEPERDEBUG_PRINT("createSession: ");
	DOORKEEPERDEBUG_PRINTLN(tmp);
	for (int i = 0; i < MAX_SRV_CLIENTS; i++) {
		if (sessions[i].name[0] == '\0') {
			memcpy(sessions[i].name, tmp, sessionid.length());
			return &sessions[i];
		}
	}
	return NULL;
}

void destroySession(DoorKeeperSession* session) {
	session->name[0] = '\0';
	DOORKEEPERDEBUG_PRINT("destroySession: ");
	DOORKEEPERDEBUG_PRINTLN(session->name);
}

void handleTelnetClients() {
	uint8_t bufferin[DoorKeeperMessageSize];
	DoorKeeperMessage* doorkeeperBufferIn = (DoorKeeperMessage*) bufferin;
	uint8_t bufferout[DoorKeeperMessageSize];
	DoorKeeperMessage* doorkeeperBufferOut = (DoorKeeperMessage*) bufferout;

	if (server.hasClient()) {
		for (int h = 0; h < MAX_SRV_CLIENTS; h++) {
			//find free/disconnected spot
			if (!serverClients[h] || !serverClients[h].connected()) {
				if (serverClients[h]) {
					serverClients[h].stop();
				}
				serverClients[h] = server.available();
				DOORKEEPERDEBUG_PRINT("New client: ");
				DOORKEEPERDEBUG_PRINTLN(h);
				break;
			}
		}
		//no free/disconnected spot so reject
		WiFiClient serverClient = server.available();
		serverClient.stop();
	}
	//check clients for data
	for (int i = 0; i < MAX_SRV_CLIENTS; i++) {
		if (serverClients[i] && serverClients[i].connected()) {

			if (serverClients[i].available()) {
				DOORKEEPERDEBUG_PRINT(serverClients[i].remoteIP().toString());
				DOORKEEPERDEBUG_PRINT(":");
				DOORKEEPERDEBUG_PRINTLN(serverClients[i].remotePort());
				//get data from the client
				DOORKEEPERDEBUG_PRINT("client read ");
				ESP.wdtFeed();
				int read = serverClients[i].read(bufferin,
						sizeof(DoorKeeperMessage));
				DOORKEEPERDEBUG_PRINT(read);
				DOORKEEPERDEBUG_PRINTLN(" bytes");
				DOORKEEPERDEBUG_PRINT("session ");
				DOORKEEPERDEBUG_PRINTLN(i);
				if (keeper.handleMessage(doorkeeperBufferIn, doorkeeperBufferOut,
						&sessions[i]) == true) {
					ESP.wdtFeed();
					// send
					sendResponse(doorkeeperBufferOut, serverClients[i]);
					ESP.wdtFeed();
					// delete buffer
					memset(doorkeeperBufferOut, 0, DoorKeeperMessageSize);
					continue;
				}
			}
			if (serverClients[i].status() == wl_tcp_state::CLOSED) {
				DOORKEEPERDEBUG_PRINTLN("client connection closed!");
				destroySession(&sessions[i]);
			}

		}


	}

}

void sendResponse(DoorKeeperMessage* response, WiFiClient client_) {

	client_.write((uint8_t*) response, (size_t) DoorKeeperMessageSize);
}

void loop() {

	handleTelnetClients();
	keeper.checkTimer();
	keeper.doorkeeperLoop();
}
