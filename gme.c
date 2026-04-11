#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <zip.h>

#define TOKEN_FILE "/home/www/data/google_access_token"
#define SHEET_ID "1RF4N-T2NR2UHai70AzTzwuLXowkLlOQWvFyb8AaE1xg"
#define SHEET_NAME "data"
#define OFFSET 20451

struct mem{
  char *ptr;
  size_t len;
};

static void mem_init(struct mem *m){
  m->len=0;
  m->ptr=(char*)malloc(1);
  if(m->ptr)m->ptr[0]='\0';
}

static size_t write_cb(void *contents,size_t size,size_t nmemb,void *userp){
  size_t real_size;
  struct mem *m;
  char *p;

  real_size=size*nmemb;
  m=(struct mem*)userp;
  p=(char*)realloc(m->ptr,m->len+real_size+1);
  if(!p)return 0;
  m->ptr=p;
  memcpy(m->ptr+m->len,contents,real_size);
  m->len=m->len+real_size;
  m->ptr[m->len]='\0';
  return real_size;
}

static int read_access_token(char *buf,size_t buf_len){
  FILE *fp;

  fp=fopen(TOKEN_FILE,"r");
  if(!fp)return 0;
  if(!fgets(buf,(int)buf_len,fp)){
    fclose(fp);
    return 0;
  }
  fclose(fp);
  buf[strcspn(buf,"\r\n")]='\0';
  return buf[0]!='\0';
}

int main(int argc,char *argv[]){
  CURL *curl;
  struct curl_slist *headers;
  struct mem auth_body;
  struct mem req_body;
  struct mem gs_body;
  char auth_post[1024];
  char req_post[2048];
  char gs_post[32768];
  char auth_header[8192];
  char gs_auth_header[2048];
  char gs_url[2048];
  char gs_range[128];
  char google_access_token[512];
  char gme_token[4096];
  char date_text[16];
  char price_values[96][64];
  char one_price[64];
  char *p;
  char *q;
  char *r;
  char *s;
  char *e;
  char *b64;
  char *json_buf;
  unsigned char *zip_buf;
  unsigned char *file_buf;
  zip_error_t zip_error;
  zip_source_t *zip_source;
  zip_t *zip_archive;
  zip_file_t *zip_file;
  zip_stat_t zip_stat;
  zip_int64_t entry_count;
  long http_code;
  int year;
  int month;
  int day;
  int day_index;
  int sheet_row;
  int len;
  int out_len;
  int val;
  int valb;
  int c;
  int i;
  int j;
  int count;
  int err;
  time_t input_time;
  time_t epoch_time;
  struct tm input_tm;
  struct tm epoch_tm;

  if(argc!=4){
    fprintf(stderr,"Usage: %s LOGIN PASSWORD YYYYMMDD\n",argv[0]);
    return 1;
  }

  if(strlen(argv[3])!=8){
    fprintf(stderr,"date format must be YYYYMMDD\n");
    return 1;
  }

  year=(argv[3][0]-'0')*1000+(argv[3][1]-'0')*100+(argv[3][2]-'0')*10+(argv[3][3]-'0');
  month=(argv[3][4]-'0')*10+(argv[3][5]-'0');
  day=(argv[3][6]-'0')*10+(argv[3][7]-'0');

  memset(&input_tm,0,sizeof(input_tm));
  input_tm.tm_year=year-1900;
  input_tm.tm_mon=month-1;
  input_tm.tm_mday=day;
  input_tm.tm_hour=12;
  input_tm.tm_min=0;
  input_tm.tm_sec=0;
  input_tm.tm_isdst=-1;

  memset(&epoch_tm,0,sizeof(epoch_tm));
  epoch_tm.tm_year=70;
  epoch_tm.tm_mon=0;
  epoch_tm.tm_mday=1;
  epoch_tm.tm_hour=12;
  epoch_tm.tm_min=0;
  epoch_tm.tm_sec=0;
  epoch_tm.tm_isdst=-1;

  input_time=mktime(&input_tm);
  epoch_time=mktime(&epoch_tm);

  if(input_time==(time_t)-1||epoch_time==(time_t)-1){
    fprintf(stderr,"date conversion error\n");
    return 1;
  }

  day_index=(int)((input_time-epoch_time)/86400);
  sheet_row=day_index-OFFSET;

  if(sheet_row<1){
    fprintf(stderr,"invalid row %d\n",sheet_row);
    return 1;
  }

  snprintf(date_text,sizeof(date_text),"%02d/%02d/%04d",day,month,year);
  snprintf(gs_range,sizeof(gs_range),"%s!A%d:CS%d",SHEET_NAME,sheet_row,sheet_row);

  if(!read_access_token(google_access_token,sizeof(google_access_token))){
    fprintf(stderr,"google access token not found\n");
    return 1;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  mem_init(&auth_body);
  mem_init(&req_body);
  mem_init(&gs_body);

  snprintf(auth_post,sizeof(auth_post),"{\"Login\":\"%s\",\"Password\":\"%s\"}",argv[1],argv[2]);

  curl=curl_easy_init();
  if(!curl){
    curl_global_cleanup();
    return 1;
  }

  headers=NULL;
  headers=curl_slist_append(headers,"Content-Type: application/json");

  curl_easy_setopt(curl,CURLOPT_URL,"https://api.mercatoelettrico.org/request/api/v1/Auth");
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,auth_post);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(auth_post));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&auth_body);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);

  if(curl_easy_perform(curl)!=CURLE_OK){
    fprintf(stderr,"auth curl error\n");
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  p=strstr(auth_body.ptr,"\"token\"");
  if(p==NULL){
    printf("%s\n",auth_body.ptr);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  p=strchr(p,':');
  p=strchr(p,'"');
  p=p+1;
  q=strchr(p,'"');

  i=0;
  while(p<q&&i<(int)sizeof(gme_token)-1){
    gme_token[i]=*p;
    i++;
    p++;
  }
  gme_token[i]=0;

  printf("TOKEN=%s\n",gme_token);

  snprintf(req_post,sizeof(req_post),
    "{\"Platform\":\"PublicMarketResults\","
    "\"Segment\":\"MGP\","
    "\"DataName\":\"ME_ZonalPrices\","
    "\"IntervalStart\":\"%s\","
    "\"IntervalEnd\":\"%s\","
    "\"Attributes\":{\"GranularityType\":\"PT15\"}}",
    argv[3],argv[3]);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  curl=curl_easy_init();
  if(!curl){
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  headers=NULL;
  headers=curl_slist_append(headers,"Content-Type: application/json");
  snprintf(auth_header,sizeof(auth_header),"Authorization: Bearer %s",gme_token);
  headers=curl_slist_append(headers,auth_header);

  curl_easy_setopt(curl,CURLOPT_URL,"https://api.mercatoelettrico.org/request/api/v1/RequestData");
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,req_post);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(req_post));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&req_body);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);

  if(curl_easy_perform(curl)!=CURLE_OK){
    fprintf(stderr,"request curl error\n");
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  p=strstr(req_body.ptr,"\"contentResponse\"");
  if(p==NULL){
    printf("%s\n",req_body.ptr);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  p=strchr(p,':');
  p=strchr(p,'"');
  p=p+1;
  q=strchr(p,'"');

  len=(int)(q-p);
  b64=(char*)malloc(len+1);
  for(i=0;i<len;i++)b64[i]=p[i];
  b64[len]=0;

  zip_buf=(unsigned char*)malloc((len*3)/4+4);
  val=0;
  valb=-8;
  out_len=0;

  for(i=0;i<len;i++){
    c=b64[i];
    if(c>='A'&&c<='Z')c=c-'A';
    else if(c>='a'&&c<='z')c=c-'a'+26;
    else if(c>='0'&&c<='9')c=c-'0'+52;
    else if(c=='+')c=62;
    else if(c=='/')c=63;
    else if(c=='=')break;
    else continue;

    val=(val<<6)+c;
    valb=valb+6;

    if(valb>=0){
      zip_buf[out_len]=(unsigned char)((val>>valb)&255);
      out_len++;
      valb=valb-8;
    }
  }

  zip_error_init(&zip_error);
  zip_source=zip_source_buffer_create(zip_buf,out_len,0,&zip_error);
  if(zip_source==NULL){
    fprintf(stderr,"zip source error\n");
    free(b64);
    free(zip_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  zip_archive=zip_open_from_source(zip_source,0,&zip_error);
  if(zip_archive==NULL){
    fprintf(stderr,"zip open error\n");
    zip_source_free(zip_source);
    free(b64);
    free(zip_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  entry_count=zip_get_num_entries(zip_archive,0);
  if(entry_count<1){
    fprintf(stderr,"zip empty\n");
    zip_close(zip_archive);
    free(b64);
    free(zip_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  zip_stat_init(&zip_stat);
  err=zip_stat_index(zip_archive,0,0,&zip_stat);
  if(err!=0){
    fprintf(stderr,"zip stat error\n");
    zip_close(zip_archive);
    free(b64);
    free(zip_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  zip_file=zip_fopen_index(zip_archive,0,0);
  if(zip_file==NULL){
    fprintf(stderr,"zip fopen error\n");
    zip_close(zip_archive);
    free(b64);
    free(zip_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  file_buf=(unsigned char*)malloc((size_t)zip_stat.size+1);
  zip_fread(zip_file,file_buf,zip_stat.size);
  file_buf[zip_stat.size]=0;
  zip_fclose(zip_file);
  zip_close(zip_archive);

  json_buf=(char*)file_buf;
  r=json_buf;
  count=0;

  while(1){
    r=strstr(r,"\"Zone\":\"PUN\"");
    if(r==NULL)break;

    s=strstr(r,"\"Price\"");
    if(s==NULL)break;

    s=strchr(s,':');
    s=s+1;
    while(*s==' ')s=s+1;
    e=s;
    while(*e!=','&&*e!='}'&&*e!=0)e=e+1;

    j=0;
    while(s<e&&j<(int)sizeof(one_price)-1){
      one_price[j]=*s;
      j++;
      s=s+1;
    }
    one_price[j]=0;

    if(count<96){
      strcpy(price_values[count],one_price);
      count++;
    }

    r=r+10;
  }

  if(count!=96){
    fprintf(stderr,"found %d prices, expected 96\n",count);
    free(b64);
    free(zip_buf);
    free(file_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  gs_post[0]=0;
  strcat(gs_post,"{\"valueInputOption\":\"USER_ENTERED\",\"data\":[{\"range\":\"");
  strcat(gs_post,gs_range);
  strcat(gs_post,"\",\"majorDimension\":\"ROWS\",\"values\":[[\"");
  strcat(gs_post,date_text);
  strcat(gs_post,"\"");

  for(i=0;i<96;i++){
    strcat(gs_post,",");
    strcat(gs_post,price_values[i]);
  }

  strcat(gs_post,"]]}]}");

  snprintf(gs_url,sizeof(gs_url),"https://sheets.googleapis.com/v4/spreadsheets/%s/values:batchUpdate",SHEET_ID);
  snprintf(gs_auth_header,sizeof(gs_auth_header),"Authorization: Bearer %s",google_access_token);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  curl=curl_easy_init();
  if(!curl){
    free(b64);
    free(zip_buf);
    free(file_buf);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  headers=NULL;
  headers=curl_slist_append(headers,"Content-Type: application/json");
  headers=curl_slist_append(headers,gs_auth_header);

  curl_easy_setopt(curl,CURLOPT_URL,gs_url);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,gs_post);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(gs_post));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&gs_body);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);

  if(curl_easy_perform(curl)!=CURLE_OK){
    fprintf(stderr,"google sheets curl error\n");
    free(b64);
    free(zip_buf);
    free(file_buf);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(auth_body.ptr);
    free(req_body.ptr);
    free(gs_body.ptr);
    return 1;
  }

  curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http_code);

  printf("%s\n",gs_body.ptr?gs_body.ptr:"");
  printf("DAY_INDEX=%d\n",day_index);
  printf("ROW=%d\n",sheet_row);
  printf("COUNT=%d\n",count);
  printf("HTTP=%ld\n",http_code);

  free(b64);
  free(zip_buf);
  free(file_buf);
  free(auth_body.ptr);
  free(req_body.ptr);
  free(gs_body.ptr);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  curl_global_cleanup();

  return 0;
}
