#include "AutoJITP.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>       //MQTT
#include <MQTTClient.h>             //MQTT
#include <ArduinoJson.h>            //JSON
#include <Preferences.h>            //Prefs
#include <Thread.h>

//Get Chip ID
String getChipNumber() {
    uint32_t chipId = 0;
    for(int i=0; i< 17; i = i+8) {
        chipId ^= ((ESP.getEfuseMac() >> (40 - i)) & 0xff0) << i;
    }
    String s = String(chipId);
    while (s.length() > 16) {
        s = s.substring(1);
    }
    while (s.length() < 4)
    {
        s = String("0") + s;
    }

    return s;
}
String getChipNumberShort() {
    String s = getChipNumber();
    return s.substring(s.length() - 4, s.length());
}

//Get Mac Address
String getMacAddress() {
    return WiFi.macAddress();
}


//Inits
// Preferences
// can't make variables a memeber of the class, aws_device_actvation_messages needs to be non member.
ProvisionStatus status;
Preferences pref;
WiFiClientSecure net;
MQTTClient client (4096);
String aws_cert;
String aws_key;
String device_name;
bool isActivated;

//Device Activation - Messages Recevied
void aws_device_actvation_messages(String &topic, String &payload);

AutoJITP::AutoJITP(){

    status = ProvisionStatus::NotStarted;
}
AutoJITP::~AutoJITP(){
    delete config;
}
MQTTClient& AutoJITP::GetClient(){
    return client;
}
void connectedClientLoopThread(){
    while(true) {
        client.loop();
        delay(100);
    }
}
void getProvisionThread();
ProvisionStatus AutoJITP::GetProvisionAsync(bool forceNewCerts)
{
    this->config = new JITPConfig();
    if (OnDeviceProvisioningProgress) OnDeviceProvisioningProgress(1);
    if (DebugStream) DebugStream->println("SetupProvision()");
    status = ProvisionStatus::InProcess;
    pref.begin("prov", false);

    isActivated = pref.getBool("activated", false);
    if (forceNewCerts) {
        isActivated = false;
        if (DebugStream)
            DebugStream->println("Forcing new certs");
    }
    if (DebugStream) DebugStream->printf("isActivated: %d\n", isActivated);
    // isActivated = false;
    // #warning forcing activation in session
    if (isActivated) {
        aws_cert = pref.getString("device_cert", "");
        aws_key = pref.getString("device_key", "");
        device_name = pref.getString("device_name", "");

        if (DebugStream) {
            DebugStream->printf("aws_cert: %s\n\n", aws_cert.c_str());
            DebugStream->printf("aws_key: %s\n\n", aws_key.c_str());
            DebugStream->printf("device_name: [%s]\n\n", device_name.c_str());
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
        Thread(connectedClientLoopThread).Start();
        //Attempt Connecting
        int aws_retries = 0;
        while (!client.connect(device_name.c_str()) && aws_retries < 20) {
            Serial.print(".");
            delay(200);
            aws_retries++;
        }    
        
        if(client.connect(device_name.c_str())) {
            if(autoJitp.DebugStream)
                autoJitp.DebugStream->printf("Connected to IoT Core @ %s\n", device_name.c_str());
        }
        else {
            if(autoJitp.DebugStream)
                autoJitp.DebugStream->printf("Could not connect to IoT Core @ %s due to: %d\n", device_name.c_str(), client.lastError());
        }
        if (autoJitp.OnProvisioned)
            autoJitp.OnProvisioned(client, device_name);
            
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
    pref.begin("prov", false);
    Thread(getProvisionThread).Start();
    return status;
}
/// @brief this is the main get provision task. Called only if the device is not already provisioned
void getProvisionThread()
{
    if (autoJitp.DebugStream) autoJitp.DebugStream->println("In Thread.");
    if (autoJitp.OnDeviceProvisioningProgress) autoJitp.OnDeviceProvisioningProgress(5);
    //Device Name
    if (autoJitp.OnDeviceProvisioningStarted)
        autoJitp.OnDeviceProvisioningStarted();
    String deviceCHIPID = getChipNumber();

    if (autoJitp.DebugStream) autoJitp.DebugStream->println("Attempt Connection");

    const char* cid = deviceCHIPID.c_str();

    bool connected = false;
    for (int aws_retries = 0; aws_retries < AWS_MAX_RECONNECT_TRIES; aws_retries++){

        if (autoJitp.OnDeviceProvisioningProgress) autoJitp.OnDeviceProvisioningProgress(5 + aws_retries);
        //Attempt Connection
        if (!client.connect(cid) && aws_retries < AWS_MAX_RECONNECT_TRIES) {
            if (autoJitp.DebugStream) autoJitp.DebugStream->println("Attempting AWS Connection");
            
            delay(1000);
            continue;
        }

        if (autoJitp.DebugStream) autoJitp.DebugStream->println("Attempt Connection done");
        //Timedout
        if(!client.connected()){
            if (autoJitp.DebugStream) autoJitp.DebugStream->println(" Timeout!");
            // Work around ????
            // tast_awsConnectForActivation.disable();

            //THROW ERROR
            //log_error(1, "AWS Connectivity: Timeout");
            status = ProvisionStatus::ConnectionError;
            if (autoJitp.OnDeviceProvisioningFailed)
                autoJitp.OnDeviceProvisioningFailed(ProvisionStatus::ConnectionError);
            return;
        }
        else
        connected = true;
        break;
    }

    if (!connected) {
        status = ProvisionStatus::ConnectionError;
        if (autoJitp.OnDeviceProvisioningFailed)
            autoJitp.OnDeviceProvisioningFailed(ProvisionStatus::ConnectionError);
        return;
    }
    if (autoJitp.OnDeviceProvisioningProgress) autoJitp.OnDeviceProvisioningProgress(50);
    if (autoJitp.DebugStream) autoJitp.DebugStream->println("Connected to AWS!: Activation Mode");

    //Receive Messages
    client.onMessage(&aws_device_actvation_messages);

    //Subscribe to Channels - For Activation
    //Step 1 - Registeration
    client.subscribe(AWS_CERT_REQUEST_REJECT);
    client.subscribe(AWS_CERT_REQUEST_ACCEPT);

    //Step 2 - Token Verification
    client.subscribe((String("$aws/provisioning-templates/") + autoJitp.config->GetAWSProvisioningTemplateName() + "/provision/json/rejected").c_str());
    client.subscribe((String("$aws/provisioning-templates/") + autoJitp.config->GetAWSProvisioningTemplateName() + "/provision/json/accepted").c_str());

    //Ping Topic for Token
    StaticJsonDocument<16> doc;

    if (autoJitp.OnDeviceProvisioningProgress) autoJitp.OnDeviceProvisioningProgress(62);
    doc["test_data"] = 1;
    String json_string;
    serializeJson(doc, json_string);
    if (autoJitp.DebugStream) autoJitp.DebugStream->printf("Cert request: %s\n", json_string.c_str());
    //Publish to Request for New Certificate
    client.publish(AWS_CERT_REQUEST_CREATE, json_string);
    long st = 0;
    while (millis() - st < AWS_Provisioning_Timeout && autoJitp.GetStatus() == ProvisionStatus::InProcess)
    {
        client.loop();
        delay(1);
    }
    if (autoJitp.GetStatus() == ProvisionStatus::InProcess) {
        status = ProvisionStatus::TimedOut;
        if (autoJitp.DebugStream) autoJitp.DebugStream->println("MQTT client timed out");
    }
    if (autoJitp.DebugStream) autoJitp.DebugStream->printf("Provisioned in: %d", millis() - st);

    return;
}

ProvisionStatus AutoJITP::GetStatus(){
    return status;
}
void aws_device_actvation_messages(String &topic, String &payload)
{
    if (autoJitp.DebugStream) autoJitp.DebugStream->printf("aws_device_actvation_messages: %s\n" , topic.c_str());
    //Registeration Token - Step 1
    if (topic.equals(AWS_CERT_REQUEST_ACCEPT)) {
        if (autoJitp.DebugStream) autoJitp.DebugStream->println("Step 1: Registeration Token");

        //Parse Payload - Deserialize
        DynamicJsonDocument doc(payload.length());
        DeserializationError error = deserializeJson(doc, payload);

        //Handle Error
        if (error) {
            if (autoJitp.DebugStream) autoJitp.DebugStream->print(F("deserializeJson() failed: "));
            if (autoJitp.DebugStream) autoJitp.DebugStream->println(error.f_str());
            //log_error(3, "AWS Device Activation: Deserialisation Failed");
            status = ProvisionStatus::Failed;
            if (autoJitp.OnDeviceProvisioningFailed)
                autoJitp.OnDeviceProvisioningFailed(ProvisionStatus::Failed);
            return;
        }

        //Save Data
        //const char* certificateId       = doc["certificateId"];
        const char* certificatePem      = doc["certificatePem"];
        const char* privateKey          = doc["privateKey"];

        //Get Token
        const char* certificateOwnershipToken = doc["certificateOwnershipToken"];

        //Save Certs
        pref.begin("prov", false);
        pref.putString("device_cert", certificatePem);
        pref.putString("device_key", privateKey);
        pref.end();

        if (autoJitp.DebugStream) autoJitp.DebugStream->printf("certificatePem: %s\n",certificatePem);
        if (autoJitp.DebugStream) autoJitp.DebugStream->printf("privateKey: %s\n", privateKey);
        //if (autoJitp.DebugStream) autoJitp.DebugStream->printf("ReadBack: %s\n", readBack.c_str());

        if (autoJitp.OnDeviceProvisioningProgress) autoJitp.OnDeviceProvisioningProgress(74);

        aws_cert = certificatePem;
        aws_key = privateKey;

        //if (DebugStream) DebugStream->println(user_token);

        //Serialize JSON - Token Verification
        DynamicJsonDocument doc_token(1024);
        doc_token["certificateOwnershipToken"]  = certificateOwnershipToken;

        JsonObject parameters       = doc_token.createNestedObject("parameters");
        if (autoJitp.OnRequestToGetProvisioningPayload){
            autoJitp.OnRequestToGetProvisioningPayload(parameters);
        }
        parameters["SerialNumber"]  = getChipNumber();
        parameters["mac_addr"]      = getMacAddress();
        parameters["chip_id"]       = getChipNumber();
        String json_string;
        serializeJson(doc_token, json_string);

        if (autoJitp.DebugStream) autoJitp.DebugStream->println(json_string);

        //Republish
        client.publish((String("$aws/provisioning-templates/") + autoJitp.config->GetAWSProvisioningTemplateName() + "/provision/json").c_str(), json_string);
    }

    //Device Registeration Confirmed - Step 2
    else if (topic.equals(String("$aws/provisioning-templates/") + autoJitp.config->GetAWSProvisioningTemplateName() + "/provision/json/accepted")) {

        if (autoJitp.OnDeviceProvisioningProgress) autoJitp.OnDeviceProvisioningProgress(86);
        if (autoJitp.DebugStream) autoJitp.DebugStream->println("Step 2: Device Registeration Confirmed");

        //Deserialize
        DynamicJsonDocument doc(200);
        deserializeJson(doc, payload);

        bool device_status = doc["deviceConfiguration"]["success"]; // true
        const char* thingName = doc["thingName"]; // "BRAP_MY_NAME_11"

        if (autoJitp.DebugStream) autoJitp.DebugStream->println("Thing Name");
        if (autoJitp.DebugStream) autoJitp.DebugStream->println(thingName);
        device_name = thingName;

        //Handle Error
        if (!device_status) {
            if (autoJitp.DebugStream) autoJitp.DebugStream->println("Device Registeration failed");
            //THROW ERROR
            //log_error(3, "AWS Device Activation: Deserialisation Failed");
            status = ProvisionStatus::Failed;
            if (autoJitp.OnDeviceProvisioningFailed)
                autoJitp.OnDeviceProvisioningFailed(ProvisionStatus::Failed);
            return;
        }

        //Device Ready - Store Settings
        pref.begin("prov", false);
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
        client.unsubscribe((String("$aws/provisioning-templates/") + autoJitp.config->GetAWSProvisioningTemplateName() + "/provision/json/rejected").c_str());
        client.unsubscribe((String("$aws/provisioning-templates/") + autoJitp.config->GetAWSProvisioningTemplateName() + "/provision/json/accepted").c_str());

        //Device Activated
        if (autoJitp.DebugStream) autoJitp.DebugStream->println("Device Activated");

        isActivated = true;

        if (autoJitp.OnDeviceProvisioningProgress) autoJitp.OnDeviceProvisioningProgress(100);
        client.disconnect(); // connect to device topic.
        if(client.connect(device_name.c_str())) {
            if(autoJitp.DebugStream)
                autoJitp.DebugStream->printf("Connected to IoT Core @ %s\n", device_name.c_str());
        }
        else {
            if(autoJitp.DebugStream)
                autoJitp.DebugStream->printf("Could not connect to IoT Core @ %s due to: %d\n", device_name.c_str(), client.lastError());
        }
        if (autoJitp.OnProvisioned)
            autoJitp.OnProvisioned(client, device_name);

        status = ProvisionStatus::Granted;
        Thread(connectedClientLoopThread).Start();
    }

    //Error
    else if (topic.equals(AWS_CERT_REQUEST_REJECT)) {
        //Request Rejected
        //gen_Error("0x1001");
        //THROW ERROR
        if (autoJitp.DebugStream) autoJitp.DebugStream->println("Certificate Rejected");
        //log_error(6, "Certificate Rejected");
        status = ProvisionStatus::Denied;
        if (autoJitp.OnDeviceProvisioningFailed)
            autoJitp.OnDeviceProvisioningFailed(ProvisionStatus::Denied);
        return;
    }

    //Error
    else if (topic.equals(String("$aws/provisioning-templates/") + autoJitp.config->GetAWSProvisioningTemplateName() + "/provision/json/rejected")) {
        //Request Rejected
        //gen_Error("0x1002");
        //THROW ERROR
        if (autoJitp.DebugStream) autoJitp.DebugStream->println("Privisioning Device Rejected");
        //log_error(6, "Privisioning Device Rejected");
        status = ProvisionStatus::Denied;
        if (autoJitp.OnDeviceProvisioningFailed)
            autoJitp.OnDeviceProvisioningFailed(ProvisionStatus::Denied);
        return;
    }
}
AutoJITP autoJitp;
