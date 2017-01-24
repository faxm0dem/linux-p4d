//***************************************************************************
// p4d / Linux - Heizungs Manager
// File webif.c
// This code is distributed under the terms and conditions of the
// GNU GENERAL PUBLIC LICENSE. See the file LICENSE for details.
// Date 04.11.2010 - 10.02.2017  Jörg Wendel
//***************************************************************************

#include <libxml/parser.h>

#include "p4d.h"

//***************************************************************************
// Class cP4WebThread
//***************************************************************************

cP4WebThread::cP4WebThread(P4d* parent)
   : cThread("Web Interface Thread", yes)
{
   p4d = parent;
   end = no;
   connection = 0;

   tableSamples = 0;
   tableMenu = 0;
   tableJobs = 0;
   tableConfig = 0;
   tableScripts = 0;
   tableTimeRanges = 0;
   tableValueFacts = 0;
   tableHmSysVars = 0;

   selectPendingJobs = 0;
   selectAllMenuItems = 0;
   selectMaxTime = 0;
   selectScriptByName = 0;
   cleanupJobs = 0;

   sem = new Sem(0x3da00001);
   serial = new Serial;
   request = new P4Request(serial);
   curl = new cCurl();
   curl->init();
}

cP4WebThread::~cP4WebThread()
{
   curl->exit();

   delete serial;
   delete request;
   delete sem;
   delete curl;
}

int cP4WebThread::start()
{
   Start(yes);
   return success;
}

int cP4WebThread::stop()
{
   end = yes;
   waitCondition.Broadcast();
   Cancel(3);
   return success;
}

int cP4WebThread::initDb()
{
   int status = success;

   if (connection)
      exitDb();

   tell(eloAlways, "Try conneting to database");

   connection = new cDbConnection();

   // ------------------------
   // create/open tables
   // ------------------------

   tableMenu = new cDbTable(connection, "menu");
   if (tableMenu->open() != success) return fail;

   tableSamples = new cDbTable(connection, "samples");
   if (tableSamples->open() != success) return fail;

   tableJobs = new cDbTable(connection, "jobs");
   if (tableJobs->open() != success) return fail;

   tableConfig = new cDbTable(connection, "config");
   if (tableConfig->open() != success) return fail;

   tableScripts = new cDbTable(connection, "scripts");
   if (tableScripts->open() != success) return fail;

   tableTimeRanges = new cDbTable(connection, "timeranges");
   if (tableTimeRanges->open() != success) return fail;

   tableValueFacts = new cDbTable(connection, "valuefacts");
   if (tableValueFacts->open() != success) return fail;

   tableHmSysVars = new cDbTable(connection, "hmsysvars");
   if (tableHmSysVars->open() != success) return fail;

   // ------------------
   // prepare statements
   // ------------------

   selectPendingJobs = new cDbStatement(tableJobs);

   selectPendingJobs->build("select ");
   selectPendingJobs->bind("ID", cDBS::bndOut);
   selectPendingJobs->bind("REQAT", cDBS::bndOut, ", ");
   selectPendingJobs->bind("STATE", cDBS::bndOut, ", ");
   selectPendingJobs->bind("COMMAND", cDBS::bndOut, ", ");
   selectPendingJobs->bind("ADDRESS", cDBS::bndOut, ", ");
   selectPendingJobs->bind("DATA", cDBS::bndOut, ", ");
   selectPendingJobs->build(" from %s where state = 'P'", tableJobs->TableName());

   status += selectPendingJobs->prepare();

   // --------------------
   // select max(time) from samples

   selectMaxTime = new cDbStatement(tableSamples);

   selectMaxTime->build("select ");
   selectMaxTime->bind("TIME", cDBS::bndOut, "max(");
   selectMaxTime->build(") from %s", tableSamples->TableName());

   status += selectMaxTime->prepare();

   // ----------------

   selectAllMenuItems = new cDbStatement(tableMenu);

   selectAllMenuItems->build("select ");
   selectAllMenuItems->bindAllOut();
   selectAllMenuItems->build(" from %s", tableMenu->TableName());

   status += selectAllMenuItems->prepare();

   // ------------------

   selectScriptByName = new cDbStatement(tableScripts);

   selectScriptByName->build("select ");
   selectScriptByName->bindAllOut();
   selectScriptByName->build(" from %s where ", tableScripts->TableName());
   selectScriptByName->bind("NAME", cDBS::bndIn | cDBS::bndSet);

   status += selectScriptByName->prepare();

   // ------------------

   cleanupJobs = new cDbStatement(tableJobs);

   cleanupJobs->build("delete from %s where ", tableJobs->TableName());
   cleanupJobs->bindCmp(0, "REQAT", 0, "<");

   status += cleanupJobs->prepare();

   // ------------------

   if (status == success)
      tell(eloAlways, "Connection to database established");

   return status;
}

int cP4WebThread::exitDb()
{
   delete tableSamples;            tableSamples = 0;
   delete tableMenu;               tableMenu = 0;
   delete tableJobs;               tableJobs = 0;
   delete tableConfig;             tableConfig = 0;
   delete tableScripts;            tableScripts = 0;
   delete tableTimeRanges;         tableTimeRanges = 0;
   delete tableValueFacts;         tableValueFacts = 0;
   delete tableHmSysVars;          tableHmSysVars = 0;

   delete selectPendingJobs;       selectPendingJobs = 0;
   delete selectAllMenuItems;      selectAllMenuItems = 0;
   delete selectMaxTime;           selectMaxTime = 0;
   delete selectScriptByName;      selectScriptByName = 0;
   delete cleanupJobs;             cleanupJobs = 0;

   delete connection;              connection = 0;

   return done;
}

//***************************************************************************
// Action
//***************************************************************************

void cP4WebThread::action()
{
   static time_t lastCleanup = time(0);
   cMyMutex mutex;

   end = no;
   mutex.Lock();

   initDb();

   while (Running() && !end)
   {
      // check db connection

      while (Running() && !end && (!connection || !connection->isConnected()))
      {
         if (initDb() != success)
         {
            exitDb();
            tell(eloAlways, "Retrying in 5 seconds");
            sleep(5);
         }
      }

      waitCondition.TimedWait(mutex, 50000);

      performRequests();

      if (lastCleanup < time(0) - 6*tmeSecondsPerHour)
      {
         cleanupWebifRequests();
         lastCleanup = time(0);
      }
   }

   exitDb();
}

//***************************************************************************
// Perform WEBIF Requests
//***************************************************************************

int cP4WebThread::performRequests()
{
   if (!connection || !connection->isConnected())
      return fail;

   tableJobs->clear();

   for (int f = selectPendingJobs->find(); f; f = selectPendingJobs->fetch())
   {
      int start = time(0);
      int addr = tableJobs->getIntValue("ADDRESS");
      const char* command = tableJobs->getStrValue("COMMAND");
      const char* data = tableJobs->getStrValue("DATA");
      int jobId = tableJobs->getIntValue("ID");

      tableJobs->find();
      tableJobs->setValue("DONEAT", time(0));
      tableJobs->setValue("STATE", "D");

      tell(eloAlways, "Processing WEBIF job %d '%s:0x%04x/%s'",
           jobId, command, addr, data);

      if (strcasecmp(command, "test-mail") == 0)
      {
         char* mailScript = 0;
         char* stateMailTo = 0;
         char* subject = strdup(data);
         char* body = 0;

         if ((body = strchr(subject, ':')))
         {
            *body = 0; body++;

            getConfigItem("mailScript", mailScript, "/usr/local/bin/p4d-mail.sh");
            getConfigItem("stateMailTo", stateMailTo);

            tell(eloDetail, "Test mail requested with: '%s/%s'", subject, body);

            if (isEmpty(mailScript))
               tableJobs->setValue("RESULT", "fail:missing mailscript");
            else if (!fileExists(mailScript))
               tableJobs->setValue("RESULT", "fail:mail-script not found");
            else if (isEmpty(stateMailTo))
               tableJobs->setValue("RESULT", "fail:missing-receiver");
            else if (sendMail(stateMailTo, subject, body, "text/plain") != success)
               tableJobs->setValue("RESULT", "fail:send failed");
            else
               tableJobs->setValue("RESULT", "success:mail sended");
         }
         else
         {
            tableJobs->setValue("RESULT", "fail:invalid request syntax");
         }
      }

      else if (strcasecmp(command, "test-alert-mail") == 0)
      {
         int id = atoi(data);

         tell(eloDetail, "Test mail for alert (%d) requested", id);

         p4d->triggerAlertCheckTestMailFor = id;
         tableJobs->setValue("RESULT", "success:check mail triggert");
      }

      else if (strcasecmp(command, "check-login") == 0)
      {
         char* webUser = 0;
         char* webPass = 0;
         md5Buf defaultPwd;

         createMd5("p4-3200", defaultPwd);

         getConfigItem("user", webUser, "p4");
         getConfigItem("passwd", webPass, defaultPwd);

         char* user = strdup(data);
         char* pwd = 0;

         if ((pwd = strchr(user, ':')))
         {
            *pwd = 0; pwd++;

            tell(eloDetail, "%s/%s", pwd, webPass);

            if (strcmp(webUser, user) == 0 && strcmp(pwd, webPass) == 0)
               tableJobs->setValue("RESULT", "success:login-confirmed");
            else
               tableJobs->setValue("RESULT", "fail:login-denied");
         }

         free(webPass);
         free(webUser);
         free(user);
      }

      else if (strcasecmp(command, "call-script") == 0)
      {
         const char* result;

         if (callScript(data, result) != success)
         {
            char* responce;
            asprintf(&responce, "fail:%s", result);
            tableJobs->setValue("RESULT", responce);
            free(responce);
         }
         else
         {
            tableJobs->setValue("RESULT", "success:done");
         }
      }

      else if (strcasecmp(command, "update-schemacfg") == 0)
      {
         p4d->triggerUpdateSchemaConf = yes;
         tableJobs->setValue("RESULT", "success:done");
      }

      else if (strcasecmp(command, "write-config") == 0)
      {
         char* name = strdup(data);
         char* value = 0;

         if ((value = strchr(name, ':')))
         {
            *value = 0; value++;

            setConfigItem(name, value);

            tableJobs->setValue("RESULT", "success:stored");
         }

         free(name);

         // read the config from table to apply changes

         p4d->triggerUpdateConfig = yes;
      }

      else if (strcasecmp(command, "read-config") == 0)
      {
         char* name = strdup(data);
         char* buf = 0;
         char* value = 0;
         char* def = 0;

         if ((def = strchr(name, ':')))
         {
            *def = 0;
            def++;
         }

         getConfigItem(name, value, def ? def : "");

         asprintf(&buf, "success:%s", value);
         tableJobs->setValue("RESULT", buf);

         free(name);
         free(buf);
         free(value);
      }

      else if (strcasecmp(command, "getp") == 0)
      {
         tableMenu->clear();
         tableMenu->setValue("ID", addr);

         if (tableMenu->find())
         {
            int type = tableMenu->getIntValue("TYPE");
            unsigned int paddr = tableMenu->getIntValue("ADDRESS");

            ConfigParameter p(paddr);

            if (getParameter(&p) == success)
            {
               char* buf = 0;
               cRetBuf value = ConfigParameter::toNice(p.value, type);

               // special for time min/max/default

               if (type == mstParZeit)
                  ;  // #TODO

               asprintf(&buf, "success#%s#%s#%d#%d#%d#%d", *value, type == 0x0a ? "Uhr" : p.unit,
                        p.def, p.min, p.max, p.digits);
               tableJobs->setValue("RESULT", buf);

               free(buf);
            }
         }
      }

      else if (strcasecmp(command, "setp") == 0)
      {
         int status;

         tableMenu->clear();
         tableMenu->setValue("ID", addr);

         if (tableMenu->find())
         {
            int type = tableMenu->getIntValue("TYPE");
            int paddr = tableMenu->getIntValue("ADDRESS");

            ConfigParameter p(paddr);

            // Set Value

            if (ConfigParameter::toValue(data, type, p.value) == success)
            {
               tell(eloAlways, "Storing value '%s/%d' for parameter at address 0x%x", data, p.value, paddr);

               if ((status = setParameter(&p)) == success)
               {
                  char* buf = 0;
                  cRetBuf value = ConfigParameter::toNice(p.value, type);

                  // store job result

                  asprintf(&buf, "success#%s#%s#%d#%d#%d#%d", *value, p.unit,
                           p.def, p.min, p.max, p.digits);
                  tableJobs->setValue("RESULT", buf);
                  free(buf);

                  // update menu table

                  tableMenu->setValue("VALUE", value);
                  tableMenu->setValue("UNIT", p.unit);
                  tableMenu->update();
               }
               else
               {
                  tell(eloAlways, "Set of parameter failed, error %d", status);

                  if (status == P4Request::wrnNonUpdate)
                     tableJobs->setValue("RESULT", "fail#no update");
                  else if (status == P4Request::wrnOutOfRange)
                     tableJobs->setValue("RESULT", "fail#out of range");
                  else
                     tableJobs->setValue("RESULT", "fail#communication error");
               }
            }
            else
            {
               tell(eloAlways, "Set of parameter failed, wrong format");
               tableJobs->setValue("RESULT", "fail#format error");
            }
         }
         else
         {
            tell(eloAlways, "Set of parameter failed, id 0x%x not found", addr);
            tableJobs->setValue("RESULT", "fail#id not found");
         }
      }

      else if (strcasecmp(command, "gettrp") == 0)
      {
         // don't update since it takes to long (assume table is up to date)
         // ### updateTimeRangeData();

         // now read it from the table

         tableTimeRanges->clear();
         tableTimeRanges->setValue("ADDRESS", addr);

         if (tableTimeRanges->find())
         {
            char* buf = 0;
            char fName[10+TB];
            char tName[10+TB];
            int n = atoi(data);

            sprintf(fName, "FROM%d", n);
            sprintf(tName, "TO%d", n);

            asprintf(&buf, "success#%s#%s#%s",
                     tableTimeRanges->getStrValue(fName),
                     tableTimeRanges->getStrValue(tName),
                     "Zeitraum");
            tableJobs->setValue("RESULT", buf);

            free(buf);
         }
      }

      else if (strcasecmp(command, "settrp") == 0)
      {
         int status = success;
         Fs::TimeRanges t(addr);
         char fName[10+TB];
         char tName[10+TB];
         int rangeNo;
         char valueFrom[100+TB];
         char valueTo[100+TB];

         // parse rangeNo and value from data

         if (sscanf(data, "%d#%[^#]#%[^#]", &rangeNo, valueFrom, valueTo) != 3)
         {
            tell(eloAlways, "Parsing of '%s' failed", data);
            status = fail;
         }

         rangeNo--;

         // get actual values from table

         tableTimeRanges->clear();
         tableTimeRanges->setValue("ADDRESS", addr);

         if (status == success && tableTimeRanges->find())
         {
            for (int n = 0; n < 4; n++)
            {
               sprintf(fName, "FROM%d", n+1);
               sprintf(tName, "TO%d", n+1);

               status += t.setTimeRange(n, tableTimeRanges->getStrValue(fName), tableTimeRanges->getStrValue(tName));
            }

            // override the 'rangeNo' with new value

            status += t.setTimeRange(rangeNo, valueFrom, valueTo);

            if (status == success)
            {
               tell(eloAlways, "Storing '%s' for time range '%d' of parameter 0x%x", t.getTimeRange(rangeNo), rangeNo+1, t.address);

               if ((status = setTimeRanges(&t)) == success)
               {
                  char* buf = 0;

                  // store job result

                  asprintf(&buf, "success#%s#%s#%s", t.getTimeRangeFrom(rangeNo), t.getTimeRangeTo(rangeNo), "Zeitbereich");
                  tableJobs->setValue("RESULT", buf);
                  free(buf);

                  // update time range table

                  sprintf(fName, "FROM%d", rangeNo+1);
                  sprintf(tName, "TO%d", rangeNo+1);
                  tableTimeRanges->setValue(fName, t.getTimeRangeFrom(rangeNo));
                  tableTimeRanges->setValue(tName, t.getTimeRangeTo(rangeNo));
                  tableTimeRanges->update();
               }
               else
               {
                  tell(eloAlways, "Set of time range parameter failed, error %d", status);

                  if (status == P4Request::wrnNonUpdate)
                     tableJobs->setValue("RESULT", "fail#no update");
                  else if (status == P4Request::wrnOutOfRange)
                     tableJobs->setValue("RESULT", "fail#out of range");
                  else
                     tableJobs->setValue("RESULT", "fail#communication error");
               }
            }
            else
            {
               tell(eloAlways, "Set of time range parameter failed, wrong format");
               tableJobs->setValue("RESULT", "fail#format error");
            }
         }
         else
         {
            tell(eloAlways, "Set of time range parameter failed, addr 0x%x for '%s' not found", addr, data);
            tableJobs->setValue("RESULT", "fail#id not found");
         }
      }

      else if (strcasecmp(command, "getv") == 0)
      {
         Value v(addr);

         tableValueFacts->clear();
         tableValueFacts->setValue("TYPE", "VA");
         tableValueFacts->setValue("ADDRESS", addr);

         if (tableValueFacts->find())
         {
            double factor = tableValueFacts->getIntValue("FACTOR");
            const char* unit = tableValueFacts->getStrValue("UNIT");

            if (getValue(&v) == success)
            {
               char* buf = 0;

               asprintf(&buf, "success:%.2f%s", v.value / factor, unit);
               tableJobs->setValue("RESULT", buf);
               free(buf);
            }
         }
      }

      else if (strcasecmp(command, "initmenu") == 0)
      {
         p4d->triggerInitMenu = yes;
         tableJobs->setValue("RESULT", "success:done");
      }

      else if (strcasecmp(command, "updatehm") == 0)
      {
         if (hmSyncSysVars() == success)
            tableJobs->setValue("RESULT", "success:done");
         else
            tableJobs->setValue("RESULT", "fail:error");
      }

      else if (strcasecmp(command, "p4d-state") == 0)
      {
         struct tm tim = {0};

         double averages[3];
         char dt[10];
         char d[100];
         char* buf;

         memset(averages, 0, sizeof(averages));
         localtime_r(&(p4d->nextAt), &tim);
         strftime(dt, 10, "%H:%M:%S", &tim);
         toElapsed(time(0)-p4d->startedAt, d);

         getloadavg(averages, 3);

         asprintf(&buf, "success:%s#%s#%s#%3.2f %3.2f %3.2f",
                  dt, VERSION, d, averages[0], averages[1], averages[2]);

         tableJobs->setValue("RESULT", buf);
         free(buf);
      }

      else if (strcasecmp(command, "s3200-state") == 0)
      {
         struct tm tim = {0};
         char date[100];
         char* buf = 0;

         localtime_r(&(p4d->currentState.time), &tim);
         strftime(date, 100, "%A, %d. %b. %G %H:%M:%S", &tim);

         asprintf(&buf, "success:%s#%d#%s#%s", date,
                  p4d->currentState.state, p4d->currentState.stateinfo,
                  p4d->currentState.modeinfo);

         tableJobs->setValue("RESULT", buf);
         free(buf);
      }

      else if (strcasecmp(command, "initvaluefacts") == 0)
      {
         p4d->triggerUpdateValueFacts = yes;
         tableJobs->setValue("RESULT", "success:done");
      }

      else if (strcasecmp(command, "updatemenu") == 0)
      {
         tableMenu->clear();

         updateMenu();
         updateTimeRangeData();

         tableJobs->setValue("RESULT", "success:done");
      }

      else
      {
         tell(eloAlways, "Warning: Ignoring unknown job '%s'", command);
         tableJobs->setValue("RESULT", "fail:unknown command");
      }

      tableJobs->store();

      tell(eloAlways, "Processing WEBIF job %d done with '%s' after %ld seconds",
           jobId, tableJobs->getStrValue("RESULT"),
           time(0) - start);
   }

   selectPendingJobs->freeResult();

   return success;
}

//***************************************************************************
// Cleanup WEBIF Requests
//***************************************************************************

int cP4WebThread::cleanupWebifRequests()
{
   int status;

   // delete jobs older than 2 days

   tell(eloAlways, "Cleanup jobs table with history of 2 days");

   tableJobs->clear();
   tableJobs->setValue("REQAT", time(0) - 2*tmeSecondsPerDay);
   status = cleanupJobs->execute();

   return status;
}

//***************************************************************************
// Call Script
//***************************************************************************

int cP4WebThread::callScript(const char* scriptName, const char*& result)
{
   int status;
   const char* path;

   result = "";

   tableScripts->clear();
   tableScripts->setValue("NAME", scriptName);

   if (!selectScriptByName->find())
   {
      selectScriptByName->freeResult();
      tell(eloAlways, "Script '%s' not found in database", scriptName);
      result = "script name not found";
      return fail;
   }

   selectScriptByName->freeResult();
   path = tableScripts->getStrValue("PATH");

   if (!fileExists(path))
   {
      tell(eloAlways, "Path '%s' not found", path);
      result = "path not found";
      return fail;
   }

   if ((status = system(path)) == -1)
   {
      tell(eloAlways, "Called script '%s' failed", path);
      return fail;
   }

   tell(eloAlways, "Called script '%s' at path '%s', exit status was (%d)", scriptName, path, status);

   return success;
}

//***************************************************************************
// Stored Parameters
//***************************************************************************

int cP4WebThread::getConfigItem(const char* name, char*& value, const char* def)
{
   free(value);
   value = 0;

   tableConfig->clear();
   tableConfig->setValue("OWNER", "p4d");
   tableConfig->setValue("NAME", name);

   if (tableConfig->find())
      value = strdup(tableConfig->getStrValue("VALUE"));
   else
   {
      value = strdup(def);
      setConfigItem(name, value);  // store the default
   }

   tableConfig->reset();

   return success;
}

int cP4WebThread::setConfigItem(const char* name, const char* value)
{
   tell(eloAlways, "Storing '%s' with value '%s'", name, value);
   tableConfig->clear();
   tableConfig->setValue("OWNER", "p4d");
   tableConfig->setValue("NAME", name);
   tableConfig->setValue("VALUE", value);

   return tableConfig->store();
}

//***************************************************************************
// Send Mail
//***************************************************************************

int cP4WebThread::sendMail(const char* receiver, const char* subject, const char* body, const char* mimeType)
{
   char* command = 0;
   char* mailScript = 0;

   getConfigItem("mailScript", mailScript, "/usr/local/bin/p4d-mail.sh");

   asprintf(&command, "%s '%s' '%s' '%s' %s", mailScript,
            subject, body, mimeType, receiver);

   system(command);
   free(command);

   tell(eloAlways, "Send mail '%s' with [%s] to '%s'",
        subject, body, receiver);

   return done;
}

//***************************************************************************
//
//***************************************************************************

int cP4WebThread::getParameter(ConfigParameter* p)
{
   int status;
   SemLock lock(sem);

   if (serial->open(ttyDeviceSvc) != success)
      return fail;

   status = request->getParameter(p);

   serial->close();

   return status;
}

int cP4WebThread::setParameter(ConfigParameter* p)
{
   int status;
   SemLock lock(sem);

   if (serial->open(ttyDeviceSvc) != success)
      return fail;

   status = request->setParameter(p);

   serial->close();

   return status;
}

int cP4WebThread::setTimeRanges(TimeRanges* t)
{
   int status;
   SemLock lock(sem);

   if (serial->open(ttyDeviceSvc) != success)
      return fail;

   status = request->setTimeRanges(t);

   serial->close();

   return status;
}

int cP4WebThread::getValue(Value* v)
{
   int status;
   SemLock lock(sem);

   if (serial->open(ttyDeviceSvc) != success)
      return fail;

   status = request->getValue(v);

   serial->close();

   return status;
}

int cP4WebThread::updateMenu()
{
   SemLock lock(sem);

   if (serial->open(ttyDeviceSvc) != success)
      return fail;

   tableMenu->clear();

   for (int f = selectAllMenuItems->find(); f; f = selectAllMenuItems->fetch())
   {
      int type = tableMenu->getIntValue("TYPE");
      int paddr = tableMenu->getIntValue("ADDRESS");


      if (type == 0x07 || type == 0x08 || type == 0x0a ||
          type == 0x40 || type == 0x39 || type == 0x32)
      {
         Fs::ConfigParameter p(paddr);

         if (request->getParameter(&p) == success)
         {
            cRetBuf value = ConfigParameter::toNice(p.value, type);

            if (tableMenu->find())
            {
               tableMenu->setValue("VALUE", value);
               tableMenu->setValue("UNIT", p.unit);
               tableMenu->update();
            }
         }
      }

      else if (type == mstFirmware)
      {
         Fs::Status s;

         if (request->getStatus(&s) == success)
         {
            if (tableMenu->find())
            {
               tableMenu->setValue("VALUE", s.version);
               tableMenu->setValue("UNIT", "");
               tableMenu->update();
            }
         }
      }

      else if (type == mstDigOut || type == mstDigIn || type == mstAnlOut)
      {
         int status;
         Fs::IoValue v(paddr);

         if (type == mstDigOut)
            status = request->getDigitalOut(&v);
         else if (type == mstDigIn)
            status = request->getDigitalIn(&v);
         else
            status = request->getAnalogOut(&v);

         if (status == success)
         {
            char* buf = 0;

            if (type == mstAnlOut)
            {
               if (v.mode == 0xff)
                  asprintf(&buf, "%d (A)", v.state);
               else
                  asprintf(&buf, "%d (%d)", v.state, v.mode);
            }
            else
               asprintf(&buf, "%s (%c)", v.state ? "on" : "off", v.mode);

            if (tableMenu->find())
            {
               tableMenu->setValue("VALUE", buf);
               tableMenu->setValue("UNIT", "");
               tableMenu->update();
            }

            free(buf);
         }
      }

      else if (type == mstMesswert || type == mstMesswert1)
      {
         int status;
         Fs::Value v(paddr);

         tableValueFacts->clear();
         tableValueFacts->setValue("TYPE", "VA");
         tableValueFacts->setValue("ADDRESS", paddr);

         if (tableValueFacts->find())
         {
            double factor = tableValueFacts->getIntValue("FACTOR");
            const char* unit = tableValueFacts->getStrValue("UNIT");

            status = request->getValue(&v);

            if (status == success)
            {
               char* buf = 0;
               asprintf(&buf, "%.2f", v.value / factor);

               if (tableMenu->find())
               {
                  tableMenu->setValue("VALUE", buf);

                  if (strcmp(unit, "°") == 0)
                     tableMenu->setValue("UNIT", "°C");
                  else
                     tableMenu->setValue("UNIT", unit);

                  tableMenu->update();
               }

               free(buf);
            }
         }
            }
   }

   selectAllMenuItems->freeResult();
   serial->close();

   return done;
}

//***************************************************************************
// Update Time Range Data
//***************************************************************************

int cP4WebThread::updateTimeRangeData()
{
   Fs::TimeRanges t;
   int status;
   char fName[10+TB];
   char tName[10+TB];
   SemLock lock(sem);

   // update / insert time ranges

   for (status = request->getFirstTimeRanges(&t); status != Fs::wrnLast; status = request->getNextTimeRanges(&t))
   {
      tableTimeRanges->clear();
      tableTimeRanges->setValue("ADDRESS", t.address);

      for (int n = 0; n < 4; n++)
      {
         sprintf(fName, "FROM%d", n+1);
         sprintf(tName, "TO%d", n+1);
         tableTimeRanges->setValue(fName, t.getTimeRangeFrom(n));
         tableTimeRanges->setValue(tName, t.getTimeRangeTo(n));
      }

      tableTimeRanges->store();
      tableTimeRanges->reset();
   }

   return done;
}

//***************************************************************************
// Synchronize HM System Variables
//***************************************************************************

int cP4WebThread::hmSyncSysVars()
{
   char* hmUrl = 0;
   char* hmHost = 0;
   xmlDoc* document = 0;
   xmlNode* root = 0;
   int readOptions = 0;
   MemoryStruct data;
   int count = 0;
   int size = 0;

#if LIBXML_VERSION >= 20900
   readOptions |=  XML_PARSE_HUGE;
#endif

   getConfigItem("hmHost", hmHost, "");

   if (isEmpty(hmHost))
      return done;

   tell(eloAlways, "Updating HomeMatic system variables");
   asprintf(&hmUrl, "http://%s/config/xmlapi/sysvarlist.cgi", hmHost);

   if (curl->downloadFile(hmUrl, size, &data) != success)
   {
      tell(0, "Error: Requesting sysvar list at homematic '%s' failed", hmUrl);
      free(hmUrl);
      return fail;
   }

   free(hmUrl);

   tell(3, "Got [%s]", data.memory ? data.memory : "<null>");

   if (document = xmlReadMemory(data.memory, data.size, "", 0, readOptions))
      root = xmlDocGetRootElement(document);

   if (!root)
   {
      tell(0, "Error: Failed to parse XML document [%s]", data.memory ? data.memory : "<null>");
      return fail;
   }

   for (xmlNode* node = root->children; node; node = node->next)
   {
      xmlChar* id = xmlGetProp(node, (xmlChar*)"ise_id");
      xmlChar* name = xmlGetProp(node, (xmlChar*)"name");
      xmlChar* type = xmlGetProp(node, (xmlChar*)"type");
      xmlChar* unit = xmlGetProp(node, (xmlChar*)"unit");
      xmlChar* visible = xmlGetProp(node, (xmlChar*)"visible");
      xmlChar* min = xmlGetProp(node, (xmlChar*)"min");
      xmlChar* max = xmlGetProp(node, (xmlChar*)"max");
      xmlChar* time = xmlGetProp(node, (xmlChar*)"timestamp");
      xmlChar* value = xmlGetProp(node, (xmlChar*)"value");

      tableHmSysVars->clear();
      tableHmSysVars->setValue("ID", atol((const char*)id));
      tableHmSysVars->find();
      tableHmSysVars->setValue("NAME", (const char*)name);
      tableHmSysVars->setValue("TYPE", atol((const char*)type));
      tableHmSysVars->setValue("UNIT", (const char*)unit);
      tableHmSysVars->setValue("VISIBLE", strcmp((const char*)visible, "true") == 0);
      tableHmSysVars->setValue("MIN", (const char*)min);
      tableHmSysVars->setValue("MAX", (const char*)max);
      tableHmSysVars->setValue("TIME", atol((const char*)time));
      tableHmSysVars->setValue("VALUE", (const char*)value);
      tableHmSysVars->store();

      xmlFree(id);
      xmlFree(name);
      xmlFree(type);
      xmlFree(unit);
      xmlFree(visible);
      xmlFree(min);
      xmlFree(max);
      xmlFree(time);
      xmlFree(value);

      count++;
   }

   tell(eloAlways, "Upate of (%d) HomeMatic system variables succeeded", count);

   return success;
}
