//***************************************************************************
// p4d / Linux - Heizungs Manager
// File p4d.h
// This code is distributed under the terms and conditions of the
// GNU GENERAL PUBLIC LICENSE. See the file LICENSE for details.
// Date 04.11.2010 - 01.03.2017  Jörg Wendel
//***************************************************************************

#ifndef _P4D_H_
#define _P4D_H_

//***************************************************************************
// Includes
//***************************************************************************

#include "lib/db.h"
#include "lib/curl.h"
#include "lib/thread.h"

#include "service.h"
#include "p4io.h"
#include "w1.h"

#include "HISTORY.h"

#define confDirDefault "/etc/p4d"

extern char dbHost[];
extern int  dbPort;
extern char dbName[];
extern char dbUser[];
extern char dbPass[];

extern char ttyDeviceSvc[];
extern int interval;
extern int stateCheckInterval;
extern int aggregateInterval;        // aggregate interval in minutes
extern int aggregateHistory;         // history in days
extern char* confDir;

class P4d;

//***************************************************************************
// Class cP4WebThread
//***************************************************************************

class cP4WebThread : public cThread, public FroelingService
{
   public:

      cP4WebThread(P4d* parent);
      ~cP4WebThread();

      int start();
      int stop();

      int initDb();
      int exitDb();

   protected:

      virtual void action();
      int performRequests();

      int cleanupWebifRequests();
      int callScript(const char* scriptName, const char*& result);

      int getConfigItem(const char* name, char*& value, const char* def = "");
      int setConfigItem(const char* name, const char* value);
      int sendMail(const char* receiver, const char* subject, const char* body, const char* mimeType);

      int getParameter(ConfigParameter* p);
      int setParameter(ConfigParameter* p);
      int setTimeRanges(TimeRanges* t);
      int getValue(Value* v);
      int updateMenu();
      int updateTimeRangeData();
      int hmSyncSysVars();

      // data

      cDbConnection* connection;

      cDbTable* tableSamples;
      cDbTable* tableMenu;
      cDbTable* tableJobs;
      cDbTable* tableConfig;
      cDbTable* tableScripts;
      cDbTable* tableSensorAlert;
      cDbTable* tableTimeRanges;
      cDbTable* tableValueFacts;
      cDbTable* tableHmSysVars;

      cDbStatement* selectPendingJobs;
      cDbStatement* selectAllMenuItems;
      cDbStatement* selectMaxTime;
      cDbStatement* selectScriptByName;
      cDbStatement* cleanupJobs;

      P4Request* request;
      Serial* serial;
      Sem* sem;
      cCurl* curl;

      int end;
      cCondVar waitCondition;
      P4d* p4d;
};

//***************************************************************************
// Class P4d
//***************************************************************************

class P4d : public FroelingService
{
   friend class cP4WebThread;

   public:

      // object

      P4d();
      ~P4d();

      int init();
	   int loop();
	   int setup();
	   int initialize(int truncate = no);

      static void downF(int aSignal) { shutdown = yes; }

   protected:

      int exit();
      int initDb();
      int exitDb();
      int readConfiguration();

      int standby(int t);
      int standbyUntil(time_t until);
      int meanwhile();

      int update();
      int updateState(Status* state);
      void scheduleTimeSyncIn(int offset = 0);
      int scheduleAggregate();
      int aggregate();

      int updateErrors();

      int store(time_t now, const char* type, int address, double value,
                unsigned int factor, const char* text = 0);

      void addParameter2Mail(const char* name, const char* value);

      void afterUpdate();
      int performAlertCheckTest();
      void sensorAlertCheck(time_t now);

      int performAlertCheck(cDbRow* alertRow, time_t now, int recurse = 0, int force = no);
      int add2AlertMail(cDbRow* alertRow, const char* title,
                            double value, const char* unit);
      int sendAlertMail(const char* to);
      int sendStateMail();
      int sendErrorMail();
      int sendMail(const char* receiver, const char* subject, const char* body, const char* mimeType);

      int updateSchemaConfTable();
      int updateValueFacts();
      int initMenu();
      int updateScripts();
      int hmUpdateSysVars();

      int isMailState();
      int loadHtmlHeader();

      int getConfigItem(const char* name, char*& value, const char* def = "");
      int setConfigItem(const char* name, const char* value);
      int getConfigItem(const char* name, int& value, int def = na);
      int setConfigItem(const char* name, int value);

      int doShutDown() { return shutdown; }

      // data

      cDbConnection* connection;

      cDbTable* tableSamples;
      cDbTable* tableValueFacts;
      cDbTable* tableMenu;
      cDbTable* tableErrors;
      cDbTable* tableJobs;
      cDbTable* tableSensorAlert;
      cDbTable* tableSchemaConf;
      cDbTable* tableSmartConf;
      cDbTable* tableConfig;
      cDbTable* tableTimeRanges;
      cDbTable* tableHmSysVars;
      cDbTable* tableScripts;

      cDbStatement* selectActiveValueFacts;
      cDbStatement* selectAllValueFacts;
      cDbStatement* selectPendingJobs;
      cDbStatement* selectAllMenuItems;
      cDbStatement* selectSensorAlerts;
      cDbStatement* selectSampleInRange;
      cDbStatement* selectPendingErrors;
      cDbStatement* selectMaxTime;
      cDbStatement* selectHmSysVarByAddr;
      cDbStatement* selectScript;

      cDbValue rangeEnd;

      time_t nextAt;
      time_t startedAt;
      Sem* sem;

      int triggerAlertCheckTestMailFor;
      int triggerUpdateSchemaConf;
      int triggerUpdateConfig;
      int triggerInitMenu;
      int triggerUpdateValueFacts;

      P4Request* request;
      Serial* serial;

      W1 w1;                       // for one wire sensors
      cCurl* curl;

      Status currentState;
      string mailBody;
      string mailBodyHtml;

      // config

      int mail;
      int htmlMail;
      char* mailScript;
      char* stateMailAtStates;
      char* stateMailTo;
      char* errorMailTo;
      int errorsPending;
      int tSync;
      time_t nextTimeSyncAt;
      int maxTimeLeak;
      MemoryStruct htmlHeader;

      string alertMailBody;
      string alertMailSubject;

      time_t nextAggregateAt;

      cMyMutex sensorAlertMutex;

      cP4WebThread* webRequestThread;

      //

      static int shutdown;
};

//***************************************************************************
#endif // _P4D_H_
