//
// Dispositivo para monitorar radiação UV com comunicação Wi-Fi
//
//  - A leitura da radiação é feita com o sensor ML8511
//  - Os dados capturados são enviados para um servidor onde as leituras são armazenadas
//  - A comunicação Wi-Fi é usado o módulo ESP-01(esp8266)
//  - O dispositivo armazena um identificador (string) na EEPROM
//  - As leituras de radiação são feitas periódicamente
//  - O dispositivo pode ser controlado usando HTTP:
//    - GET /read
//      Realiza uma leitura retornando o valor.
//    - GET /reset
//      Reseta o dispositvo.
//    - GET /id
//      Devolve o ID atual do dispositivo
//    - GET /id/{novo_id}
//      Configura um novo id para o dispositivo
//


#include <SoftwareSerial.h>
#include <avr/wdt.h>
#include <EEPROM.h>


#define WIFI_SSID       "SSID"                    // SSID da rede wifi para se conectar
#define WIFI_PASSWORD   "SENHA"                   // senha da rede wifi
#define REMOTE_URL      "monitoruv.herokuapp.com" //endereço do servidor
#define REMOTE_PORT     80                        // porta do servidor
#define EEPROM_ID_ADDR  0                         // endereço de armazenamento do identificador do dispositivo
#define DEBUG           true                      // modo de debug da rede(mostra todos os comandos)
#define SERVER_TIME     600000                    // 10 minutos (10 * 60secs * 1000)rodando o server para atender as requisições

#define ARDUINO_RX_ESP_TX   2   // pino onde o esp envia dado para o arduino ler
#define ARDUINO_TX_ESP_RX   3   // pino onde o arduino envia dado para o esp ler

#define UV_READING          A0  // pino onde será realizada a leitura do sensor UV
#define UV_REF              A1  // pino usado como referência de voltagem para o mapeamento p/ indice UV



SoftwareSerial esp8266(ARDUINO_RX_ESP_TX, ARDUINO_TX_ESP_RX);

// pinos usados pelo sensor UV ML8511
int PIN_UV  = UV_READING;  // pino da leitura do sensor
int PIN_3V3 = UV_REF; // leitura de referência da alimentação 3V3

String deviceID = "";


//--------------------------------------
// Principal
//--------------------------------------


// função de inicialização
void setup()
{
  Serial.begin(9600);

  // configuração dos pinos do ML8511
  pinMode(PIN_UV, INPUT);
  pinMode(PIN_3V3, INPUT);

  // configuração do módulo Wi-fi
  esp8266.begin(9600);
  configWifi();

  //lê o nome do dispositivo da EEPROM
  readID();
}


// laço principal do arduino
void loop()
{
  // envia uma leitura para o servidor
  uploadReading();

  //configWifi();
  // liga o servidor escutando a porta 80
  sendData("AT+CIPSERVER=1,80\r\n", 1000, DEBUG);

  // espera rodando o servidor
  unsigned long serverTime = SERVER_TIME + millis();
  while ( serverTime > millis())
  {
    server();
  }
}

//--------------------------------------
// Função de envio da leitura
//--------------------------------------

void uploadReading()
{
  // assegura que a conexão 0 está livre pra ser usada
  sendData("AT+CIPCLOSE=0\r\n", 2000, DEBUG);

  // monta o comando para abrir a conexão
  String atStart = "AT+CIPSTART=0,\"TCP\",\"";
  atStart += REMOTE_URL;
  atStart += "\",";
  atStart += REMOTE_PORT;
  atStart += "\r\n";

  // envia o comando de abertura de conexão
  sendData(atStart, 1000, DEBUG);
  delay(1000);

  // monta a requisição
  String request = "POST /api/dados/ HTTP/1.1\r\nHost: ";
  request += REMOTE_URL;
  request += "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ";

  //realiza a leitura UV
  int uvindex = readUV();

  // monta o conteúdo da requisição
  String content = "leituraUV=";
  content += uvindex;
  content += "&dispositivo=";
  content += deviceID;

  // finalização a requisição
  request += content.length();
  request += "\r\n\r\n";
  request += content;
  request += "\r\n";

  // Comando de envio da requisição
  String command = "AT+CIPSEND=0,";
  command += request.length();
  command += "\r\n";

  // envia a requisição
  sendData(command, 1000, DEBUG);
  sendData(request, 1000, DEBUG);

  delay(5000);

  // fecha a conexão, deve retornar erro por a conexão já ter sido fechada pelo servidor
  sendData("AT+CIPCLOSE=0\r\n", 1000, DEBUG);
}


//--------------------------------------
// Funções do Servidor
//--------------------------------------


// função principal do servidor
void server()
{
  // verifica se há dados na via serial para serem lidos
  if(esp8266.available())
  {
    // exemplo de requisição
    //  +IPD,0,77:GET / HTTP/1.1
    //  Host: 192.168.0.114
    //  ...

    // procura o inicio da requisição
    if(esp8266.find("+IPD,"))
    {
      // delay pra acumular a requisição
      delay(1000);
      // pega o id da conexão
      int connId = esp8266.read() -48;

      // mostra o id no monitor serial
      //Serial.print("Conexao #");
      //Serial.print(connId);

      serverResolveUrl(connId);
    }
  }
}


// função para resolver as urls nas requisições
void serverResolveUrl(int connId)
{
  // busca o método get na requisição
  if(esp8266.find("GET /"))
  {
    // url o comando do metodo
    String url = esp8266.readStringUntil(' ');

    // indice para busca em url complexa
    int index = -1;

    if(url == "read")
    {
      serverResponse(connId, "200 OK", serverReadUVResponse());
    }
    else if(url == "reset")
    {
      serverResponse(connId, "200 OK", "Restarting...\n");

      // reinicia o arduino
      deviceReset();
    }
    else if(url == "id")
    {
      serverResponse(connId, "200 OK", serverGetDeviceID());
    }
    else if((index = url.indexOf("id/")) != -1)
    {
      String newId = url.substring(index+3);
      changeId(newId);
      serverResponse(connId, "200 OK", serverGetDeviceID());
    }
    else
    {
      //Serial.println("CMD: ERROR");
      serverErrorResponse(connId, "Invalid URL!\n");
    }
  }
  else
  {
    //Serial.println("METHOD ERROR");
    serverErrorResponse(connId, "Incorrect Method!\n");
  }
}


// pega a leitura do sensor e monta a resposta
String serverReadUVResponse()
{
  // faz a leitura do sensor
  int uvIndex = (int)readUV();

  String res = "UVi:";
  res += String(uvIndex) + "\n";

  return res;
}


// pega o identificador do dispositivo e monta uma resposta
String serverGetDeviceID()
{
  return deviceID + "\n";
}


// envia uma resposta de erro
void serverErrorResponse(int connId, String errorMsg)
{
  serverResponse(connId, "404 Not Found", errorMsg);
}


// envia uma resposta
// o código da resposta é como:
// "404 Not Found"
// "200 OK"
void serverResponse(int connId, String resCode, String content)
{
  // monta a resposta
  String resp = "HTTP/1.1 ";
  resp += resCode;
  resp += "\r\nContent-Type: text/html\r\nContent-Length: ";
  resp += content.length();
  resp += "\r\n\r\n";
  resp += content;
  
  // comando do envio de dados
  String cmdSend = "AT+CIPSEND=";
  cmdSend += connId;
  cmdSend += ",";
  cmdSend += resp.length();
  cmdSend += "\r\n";

  sendData(cmdSend, 1000, DEBUG);
  sendData(resp, 1000, DEBUG);  
}

//--------------------------------------
// Funções do sensor de de radiação ultravioleta(ML8511)
//--------------------------------------


// lê o valor do sensor retornando o índice uv equivalente
float readUV()
{
  int uvVolt = averageAnalogRead(PIN_UV);
  int ref3v3 = averageAnalogRead(PIN_3V3);

  // usa referência pra "melhorar" a leitura
  float voltage = 3.3f / ref3v3 * uvVolt;

  // mapeia a leitura para o índice equivalente usando a
  // tabela de referência no datasheet do sensor
  float uvindex = mapfloat(voltage, 0.99, 2.9, 0.0, 15.0);

  return uvindex;
}


// mapeia os valores minimo e máximo de voltagem para dentro da escala do índice UV
float mapfloat(float x, float in_min, float in_max, float out_min, float out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}


// faz a média dos valores de 8 leituras, para evitar erros
int averageAnalogRead(int pinToRead)
{
  byte numberOfReadings = 8;
  unsigned int runningValue = 0; 

  for(int x = 0 ; x < numberOfReadings ; x++)
    runningValue += analogRead(pinToRead);
  runningValue /= numberOfReadings;

  return(runningValue);  
}


//--------------------------------------
// Funções de Wi-fi (Módulo ESP-01 (ESP8266))
//--------------------------------------


// configura a conexão Wi-fi
void configWifi()
{  
  sendData("AT+RST\r\n", 2000, DEBUG); // rst
  // Conecta a rede wireless
  String ap = "AT+CWJAP=\"";
  ap += WIFI_SSID;
  ap += "\",\"";
  ap += WIFI_PASSWORD;
  ap += "\"\r\n";
  sendData(ap, 2000, DEBUG);
  delay(3000);
  // modo wifi número 1(estação-cliente)
  sendData("AT+CWMODE=1\r\n", 1000, DEBUG);
  // Mostra o endereco IP
  sendData("AT+CIFSR\r\n", 1000, DEBUG);
  // Configura para multiplas conexoes(max=4)
  sendData("AT+CIPMUX=1\r\n", 1000, DEBUG);
}


// envia dados para o módulo
String sendData(String command, const int timeout, boolean debug)
{
  Serial.print(command);
  // Envio dos comandos AT para o modulo
  String response = "";
  esp8266.print(command);
  long int time = millis();
  while ( (time + timeout) > millis())
  {
    while (esp8266.available())
    {
      // The esp has data so display its output to the serial window
      char c = esp8266.read(); // read the next character.
      response += c;
    }
  }
  if (debug)
  {
    Serial.print(response);
  }
  return response;
}


//--------------------------------------
// Funções Auxiliares do Arduino
//--------------------------------------

// função para reiniciar o dispositivo
void deviceReset()
{
  //reseta o arduino
  wdt_enable(WDTO_30MS);
}


// lê o ID armazenado na EEPROM
void readID()
{
  deviceID = eepromReadString(EEPROM_ID_ADDR);
  Serial.print("Device ID:");
  Serial.println(deviceID);
}


// troca o identificador do dispositivo
void changeId(String newId)
{
  eepromWriteString(newId, EEPROM_ID_ADDR);
  readID();
}


// lê uma String terminada em '\0' a partir do endereço especificado da EEPROM
String eepromReadString(int addr)
{
  String resp;
  char c;
  int i;
  
  while((c = EEPROM.read(addr + i++)) != '\0')
  {
    resp += c;
  }

  return resp;
}


// grava os bytes de uma string na EEPROM
void eepromWriteString(String str, int addr)
{
  for(int i = 0; i < str.length(); i++)
  {
    EEPROM.write(addr + i, str[i]);
  }
    
  EEPROM.write(addr + str.length(), '\0');
}
