#include "JITP.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>       //MQTT
#include <MQTTClient.h>             //MQTT
#include "Common/Logs.h"
#include <ArduinoJson.h>            //JSON
#include <Preferences.h>            //Prefs
#include <Thread.h>
#include "Helpers/general.h"
#include <LittleFS.h>

enum ProvisionStatus:byte{
    Granted,
    Denied,
    ConnectionError,
    InProcess,
    NotStarted,
    TimedOut,
    Failed
};

//Inits
// Preferences
// can't make variables a memeber of the class, aws_device_actvation_messages needs to be non member.
ProvisionStatus status;
Preferences pref;
WiFiClientSecure net;
MQTTClient client (4096);
String token;
String aws_cert;
String aws_key;
String device_name;
bool isActivated;

//Device Activation - Messages Recevied
void aws_device_actvation_messages(String &topic, String &payload);

JITP::JITP(){

    status = ProvisionStatus::NotStarted;
}
JITP::~JITP(){
    delete config;
}
MQTTClient& JITP::GetClient(){
    return client;
}
void getProvisionThread();
ProvisionStatus JITP::GetProvisionAsync(bool forceNewCerts)
{
    this->config = new JITPConfig();
    if (OnDeviceProvisioningProgress) OnDeviceProvisioningProgress(1);
    if (DebugStream) DebugStream->println("SetupProvision()");
    status = ProvisionStatus::InProcess;
    pref.begin("prov.sys", false);
    token = pref.getString("user_token", "");

    //Generate Random Token, if it Does Not Exists
    if (token.length() == 0) {
        if (DebugStream) DebugStream->println("No User Token Found");
        int randNumber = random(999999);
        token = String(randNumber);
        pref.putString("user_token", token);
    }


    isActivated = pref.getBool("activated", false);
    isActivated = false;
    if (forceNewCerts) {
        isActivated = false;
        DebugStream->println("Forcing new certs");
    }
    if (DebugStream) DebugStream->printf("isActivated: %d\n", isActivated);
    // isActivated = false;
    // #warning forcing activation in session
    if (isActivated) {
        aws_cert = pref.getString("device_cert", "");
        //aws_key = pref.getString("device_key", "");
        device_name = pref.getString("device_name", "");

        // prefs too small for this
        File f = LittleFS.open("/device_key.sys","r");
        char* buffer = new char [f.size() + 1];
        f.readBytes(buffer, f.size());
        buffer[f.size()] = 0;
        f.close();
        aws_key = String(buffer);
        delete buffer;

        if (DebugStream) {
            DebugStream->printf("aws_cert: %s\n\n", aws_cert.c_str());
            DebugStream->printf("aws_key: %s\n\n", aws_key.c_str());
            DebugStream->printf("device_name: %s\n\n", device_name.c_str());
        }


        // Configure WiFiClientSecure to use the AWS IoT device credentials
        net.setCACert(config->GetAWSRootCA());
        net.setCertificate(aws_cert.c_str());
        net.setPrivateKey(aws_key.c_str());

        client.begin(config->GetAWSEndPoint(), 8883, net);
        if (DebugStream) DebugStream->printf("Already granted\n");
        pref.end();
        status = ProvisionStatus::Granted;
        if (OnDeviceProvisioningProgress) OnDeviceProvisioningProgress(100);
        if (jitp.OnProvisioned)
            jitp.OnProvisioned(client);
        return ProvisionStatus::Granted;
    }

    if (DebugStream) {
        DebugStream->println("Setting root CA");
        DebugStream->printf("Root CA: %s\n", config->GetAWSRootCA());
        DebugStream->printf("Cert: %s\n", config->GetAWSInitialCertificate());
        DebugStream->printf("Key: %s\n", config->GetAWSInitialKey());
        DebugStream->printf("EndPoint: %s\n", config->GetAWSEndPoint());
    }
    //Set Factory Certs
    net.setCACert(config->GetAWSRootCA());
    net.setCertificate(config->GetAWSInitialCertificate());
    net.setPrivateKey(config->GetAWSInitialKey());
    client.begin(config->GetAWSEndPoint(), 8883, net);

    if (DebugStream) DebugStream->println("Root CA set. Starting the provisioning thread");
    // Begin async process
    pref.begin("prov.sys", false);
    Thread(getProvisionThread).Start();
    return status;
}
/// @brief this is the main get provision task. Called only if the device is not already provisioned
void getProvisionThread()
{
    if (jitp.DebugStream) jitp.DebugStream->println("In Thread.");
    if (jitp.OnDeviceProvisioningProgress) jitp.OnDeviceProvisioningProgress(5);
    //Device Name
    if (jitp.OnDeviceProvisioningStarted)
        jitp.OnDeviceProvisioningStarted();
    String deviceCHIPID = getChipNumber();

    if (jitp.DebugStream) jitp.DebugStream->println("Attempt Connection");

    const char* cid = deviceCHIPID.c_str();

    for (int aws_retries =0; aws_retries < AWS_MAX_RECONNECT_TRIES; aws_retries++){

        if (jitp.OnDeviceProvisioningProgress) jitp.OnDeviceProvisioningProgress(5 + aws_retries);
        //Attempt Connection
        if (!client.connect(cid) && aws_retries < AWS_MAX_RECONNECT_TRIES) {
            if (jitp.DebugStream) jitp.DebugStream->println("Attempting AWS Connection");
            aws_retries++;
            continue;
        }

        if (jitp.DebugStream) jitp.DebugStream->println("Attempt Connection done");
        //Timedout
        if(!client.connected()){
            if (jitp.DebugStream) jitp.DebugStream->println(" Timeout!");
            // Work around ????
            // tast_awsConnectForActivation.disable();

            //THROW ERROR
            log_error(1, "AWS Connectivity: Timeout");
            status = ProvisionStatus::ConnectionError;
            if (jitp.OnDeviceProvisioningFailed)
                jitp.OnDeviceProvisioningFailed(ProvisionStatus::ConnectionError);
            return;
        }
        else
        break;
    }

    if (jitp.OnDeviceProvisioningProgress) jitp.OnDeviceProvisioningProgress(50);
    if (jitp.DebugStream) jitp.DebugStream->println("Connected to AWS!: Activation Mode");

    //Receive Messages
    client.onMessage(&aws_device_actvation_messages);

    //Subscribe to Channels - For Activation
    //Step 1 - Registeration
    client.subscribe(AWS_CERT_REQUEST_REJECT);
    client.subscribe(AWS_CERT_REQUEST_ACCEPT);

    //Step 2 - Token Verification
    client.subscribe((String("$aws/provisioning-templates/") + jitp.config->GetAWSProvisioningTemplateName() + "/provision/json/rejected").c_str());
    client.subscribe((String("$aws/provisioning-templates/") + jitp.config->GetAWSProvisioningTemplateName() + "/provision/json/accepted").c_str());

    //Ping Topic for Token
    StaticJsonDocument<16> doc;

    if (jitp.OnDeviceProvisioningProgress) jitp.OnDeviceProvisioningProgress(62);
    doc["test_data"] = 1;
    String json_string;
    serializeJson(doc, json_string);
    if (jitp.DebugStream) jitp.DebugStream->printf("Cert request: %s\n", json_string.c_str());
    //Publish to Request for New Certificate
    client.publish(AWS_CERT_REQUEST_CREATE, json_string);
    long st = 0;
    while (millis() - st < AWS_Provisioning_Timeout && jitp.GetStatus() == ProvisionStatus::InProcess)
    {
        client.loop();
        delay(1);
    }
    if (jitp.GetStatus() == ProvisionStatus::InProcess) {
        status = ProvisionStatus::TimedOut;
        if (jitp.DebugStream) jitp.DebugStream->println("MQTT client timed out");
    }
    if (jitp.DebugStream) jitp.DebugStream->printf("Provisioned in: %d", millis() - st);

    return;
}

ProvisionStatus JITP::GetStatus(){
    return status;
}
void aws_device_actvation_messages(String &topic, String &payload)
{
    if (jitp.DebugStream) jitp.DebugStream->printf("aws_device_actvation_messages: %s\n" , topic.c_str());
    //Registeration Token - Step 1
    if (topic.equals(AWS_CERT_REQUEST_ACCEPT)) {
        if (jitp.DebugStream) jitp.DebugStream->println("Step 1: Registeration Token");

        //Parse Payload - Deserialize
        DynamicJsonDocument doc(payload.length());
        DeserializationError error = deserializeJson(doc, payload);

        //Handle Error
        if (error) {
            if (jitp.DebugStream) jitp.DebugStream->print(F("deserializeJson() failed: "));
            if (jitp.DebugStream) jitp.DebugStream->println(error.f_str());
            log_error(3, "AWS Device Activation: Deserialisation Failed");
            status = ProvisionStatus::Failed;
            if (jitp.OnDeviceProvisioningFailed)
                jitp.OnDeviceProvisioningFailed(ProvisionStatus::Failed);
            return;
        }

        //Save Data
        //const char* certificateId       = doc["certificateId"];
        const char* certificatePem      = doc["certificatePem"];
        const char* privateKey          = doc["privateKey"];

        //Get Token
        const char* certificateOwnershipToken = doc["certificateOwnershipToken"];

        //Save Certs
        pref.begin("prov.sys", false);
        pref.putString("device_cert", certificatePem);
        pref.end();
        // prefs too small for this
        File f = LittleFS.open("/device_key.sys","w");
        f.print(privateKey);
        f.close();
        if (jitp.DebugStream) jitp.DebugStream->printf("f1 size: %d\n", f.size());


        // File f2 = LittleFS.open("/device_key.sys","r");
        // Serial.printf("f2 size: %d\n", f2.size());
        // char* buffer = new char [f2.size() + 1];
        // f2.readBytes(buffer, f2.size());
        // buffer[f2.size()] = 0;
        // f2.close();
        // String readBack = String(buffer);

        if (jitp.DebugStream) jitp.DebugStream->printf("certificatePem: %s\n",certificatePem);
        if (jitp.DebugStream) jitp.DebugStream->printf("privateKey: %s\n", privateKey);
        //if (jitp.DebugStream) jitp.DebugStream->printf("ReadBack: %s\n", readBack.c_str());

        if (jitp.OnDeviceProvisioningProgress) jitp.OnDeviceProvisioningProgress(74);

        aws_cert = certificatePem;
        aws_key = privateKey;

        //if (DebugStream) DebugStream->println(user_token);

        //Serialize JSON - Token Verification
        DynamicJsonDocument doc_token(1024);
        doc_token["certificateOwnershipToken"]  = certificateOwnershipToken;

        JsonObject parameters       = doc_token.createNestedObject("parameters");
        parameters["SerialNumber"]  = getChipNumber();
        parameters["mac_addr"]      = getMacAddress();
        parameters["chip_id"]       = getChipNumber();
        parameters["token"]         = token;
        String json_string;
        serializeJson(doc_token, json_string);

        if (jitp.DebugStream) jitp.DebugStream->println(json_string);

        //Republish
        client.publish((String("$aws/provisioning-templates/") + jitp.config->GetAWSProvisioningTemplateName() + "/provision/json").c_str(), json_string);
    }

    //Device Registeration Confirmed - Step 2
    else if (topic.equals(String("$aws/provisioning-templates/") + jitp.config->GetAWSProvisioningTemplateName() + "/provision/json/accepted")) {

        if (jitp.OnDeviceProvisioningProgress) jitp.OnDeviceProvisioningProgress(86);
        if (jitp.DebugStream) jitp.DebugStream->println("Step 2: Device Registeration Confirmed");

        //Deserialize
        DynamicJsonDocument doc(200);
        deserializeJson(doc, payload);

        bool device_status = doc["deviceConfiguration"]["success"]; // true
        const char* thingName = doc["thingName"]; // "BRAP_MY_NAME_11"

        if (jitp.DebugStream) jitp.DebugStream->println("Thing Name");
        if (jitp.DebugStream) jitp.DebugStream->println(thingName);
        device_name = thingName;

        //Handle Error
        if (!device_status) {
            if (jitp.DebugStream) jitp.DebugStream->println("Device Registeration failed");
            //THROW ERROR
            log_error(3, "AWS Device Activation: Deserialisation Failed");
            status = ProvisionStatus::Failed;
            if (jitp.OnDeviceProvisioningFailed)
                jitp.OnDeviceProvisioningFailed(ProvisionStatus::Failed);
            return;
        }

        //Device Ready - Store Settings
        pref.begin("prov.sys", false);
        pref.putBool("activated", true);
        pref.putString("device_name", thingName);
        pref.end();

        //Disconnect Current Session
        client.disconnect();

        //Unsubscribe
        //Step 1
        client.unsubscribe(AWS_CERT_REQUEST_REJECT);
        client.unsubscribe(AWS_CERT_REQUEST_ACCEPT);

        //Step 2
        client.unsubscribe((String("$aws/provisioning-templates/") + jitp.config->GetAWSProvisioningTemplateName() + "/provision/json/rejected").c_str());
        client.unsubscribe((String("$aws/provisioning-templates/") + jitp.config->GetAWSProvisioningTemplateName() + "/provision/json/accepted").c_str());

        //Device Activated
        if (jitp.DebugStream) jitp.DebugStream->println("Device Activated");

        isActivated = true;

        if (jitp.OnDeviceProvisioningProgress) jitp.OnDeviceProvisioningProgress(100);
        if (jitp.OnProvisioned)
            jitp.OnProvisioned(client);

        status = ProvisionStatus::Granted;
    }

    //Error
    else if (topic.equals(AWS_CERT_REQUEST_REJECT)) {
        //Request Rejected
        //gen_Error("0x1001");
        //THROW ERROR
        if (jitp.DebugStream) jitp.DebugStream->println("Certificate Rejected");
        log_error(6, "Certificate Rejected");
        status = ProvisionStatus::Denied;
        if (jitp.OnDeviceProvisioningFailed)
            jitp.OnDeviceProvisioningFailed(ProvisionStatus::Denied);
        return;
    }

    //Error
    else if (topic.equals(String("$aws/provisioning-templates/") + jitp.config->GetAWSProvisioningTemplateName() + "/provision/json/rejected")) {
        //Request Rejected
        //gen_Error("0x1002");
        //THROW ERROR
        if (jitp.DebugStream) jitp.DebugStream->println("Privisioning Device Rejected");
        log_error(6, "Privisioning Device Rejected");
        status = ProvisionStatus::Denied;
        if (jitp.OnDeviceProvisioningFailed)
            jitp.OnDeviceProvisioningFailed(ProvisionStatus::Denied);
        return;
    }
}
JITP jitp;
