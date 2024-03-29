#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>       //MQTT
#include <MQTTClient.h>             //MQTT
#include <ArduinoJson.h>            //JSON
#include <Preferences.h>            //Prefs

void getProvisionCStyle();
enum ProvisionStatus:byte{
    Granted,
    Denied,
    ConnectionError,
    InProcess,
    NotStarted,
    TimedOut,
    Failed
};
#define AWS_MAX_RECONNECT_TRIES 10
#define AWS_Provisioning_Timeout 30000
//Device Setup Topics - Subscribe
#define AWS_CERT_REQUEST_ACCEPT "$aws/certificates/create/json/accepted"
#define AWS_CERT_REQUEST_REJECT "$aws/certificates/create/json/rejected"

//#define AWS_CERT_PROVISION_ACCEPT "$aws/provisioning-templates/BatteryX_Provision/provision/json/accepted"
//#define AWS_CERT_PROVISION_REJECT "$aws/provisioning-templates/BatteryX_Provision/provision/json/rejected"

//Device Setup Topics - Publish
#define AWS_CERT_REQUEST_CREATE "$aws/certificates/create/json"
//#define AWS_CERT_REQUEST_PROVISION "$aws/provisioning-templates/BatteryX_Provision/provision/json"
// #define AWS_IOT_ENDPOINT "a2q3xq7o4p30im-ats.iot.us-east-1.amazonaws.com" // BreatheIO
//#define AWS_IOT_ENDPOINT "a2fqhpcrad2mxw-ats.iot.us-east-1.amazonaws.com" // Mubeen


class JITPConfig {
    public:
    const char* GetAWSEndPoint();
    const char* GetAWSRootCA();
    const char* GetAWSInitialCertificate();
    const char* GetAWSInitialKey();
    const char* GetAWSProvisioningTemplateName();
};

class AutoJITP
{
public:
    AutoJITP();
    ~AutoJITP();
    /// @brief Gets IoT core provision from AWS, sets up Thing name.
    /// @return ProvisionStatus::Granted if already provisioned or ProvisionStatus::InProcess and begins async provision process in parallel.
    ProvisionStatus GetProvisionAsync(bool forceNewCerts = false, bool dontRunClientLoop = false);
    /// @brief Gets the provisioning status at any time.
    /// @return Provision Status
    ProvisionStatus GetStatus();
    /// @brief Called when an existing or newly acquired provisioning is acquired.
    void (*OnProvisioned)(MQTTClient &client, String& deviceName) = 0;
    /// @brief During the aync routine of SetupProvision, this event is called when Online provisioning begins
    void (*OnDeviceProvisioningStarted)() = 0;
    /// @brief During the aync routine of SetupProvision, this event is called before retrying to get provision
    void (*OnDeviceProvisioningRetry)(int retryCount) = 0;
    /// @brief Called when the provisioning finally fails
    void (*OnDeviceProvisioningFailed)(ProvisionStatus cause) = 0;
    /// @brief Called when some progress is made during provisioning scale [0-100]
    void (*OnDeviceProvisioningProgress)(int progress) = 0;
    /// @brief Additional payload that the provisioning lambda might need
    void (*OnRequestToGetProvisioningPayload)(JsonObject& payload) = 0;
    MQTTClient& GetClient();
    Stream* DebugStream = 0;
    JITPConfig* config;
private:
    //AWS Activate Device - Connect
    void awsConnectForActivation();
};

extern AutoJITP autoJitp;
