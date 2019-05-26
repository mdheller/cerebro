/*
Copyright (c) 2017, The University of Bristol, Senate House, Tyndall Avenue, Bristol, BS8 1TH, United Kingdom.
Copyright (c) 2018, COSIC-KU Leuven, Kasteelpark Arenberg 10, bus 2452, B-3001 Leuven-Heverlee, Belgium.

All rights reserved
Ryan Deng was here.
*/

#include "offline_FHE.h"
#include "config.h"
#include "Tools/ThreadPool.h"
#include <future>
#include <omp.h>
#include <sstream> 
using std::future;
using std::cout;
using std::stringstream;

void DistDecrypt(Plaintext &mess, const Ciphertext &ctx, const FHE_SK &sk, Player &P)
{
  ThreadPool pool(64);
  vector<future<void>> res;
  const FHE_Params &params= ctx.get_params();

  vector<bigint> vv(params.phi_m());
  sk.dist_decrypt_1(vv, ctx, P.whoami());

  // Now pack into a string  for broadcasting
  vector<string> vs(P.nplayers());
  for (unsigned int i= 0; i < params.phi_m(); i++)
    {
      outputBigint(vs[P.whoami()], vv[i]);
    }

  // Broadcast and Receive the values, change it to be multithreaded.
  // P.broadcast_receive(vs);


  // Receive
  for (int i = 0; i < P.nplayers(); i++) {
    if (i != P.whoami()) {
      res.push_back(pool.enqueue([i, &P, &vs]() {
        string recv;
        P.receive_from_player(i, recv);
        vs[i] = recv;
      }));
    }
  }
  // Send
  for (int i = 0; i < P.nplayers(); i++) {
    if (i != P.whoami()) {
      res.push_back(pool.enqueue([i, &params, &P, &vs]() {
        P.send_to_player(i, vs[P.whoami()]);
      }));
    }
  }

  joinNclean(res);
  printf("Before Reconstruction\n");
  // Reconstruct the value mod p0 from all shares
  vector<bigint> vv1(params.phi_m());
  for (unsigned int i= 0; i < P.nplayers(); i++)
    {
      if (i != P.whoami())
      {
        for (unsigned int j= 0; j < params.phi_m(); j++)
          {
            inputBigint(vs[i], vv1[j]);
          }
      }
      sk.dist_decrypt_2(vv, vv1); 
    }

  printf("After reconstruction\n");
  // Now get the final message
  bigint mod= params.p0();
  mess.set_poly_mod(vv, mod);
}

/* This one generates a new fresh ciphertext encrypting the
 * same values as the input ciphertex cm does
 *   - m is my share
 */
void Reshare(Plaintext &m, Ciphertext &cc, const Ciphertext &cm,
             Player &P, const FHE_PK &pk, const FHE_SK &sk,
             FHE_Industry &industry)
{
  Plaintext f(m.get_field());
  Ciphertext cf(cm.get_params());

  industry.Next_Off_Production_Line(f, cf, P, "Reshare");

  // We could be resharing a level 0 ciphertext so adjust if we are
  if (cm.level() == 0)
    {
      cf.Scale(m.get_field().get_prime());
    }

  // Step 4
  Ciphertext cmf(cm.get_params());
  add(cmf, cf, cm);

  // Step 5
  Plaintext mf(m.get_field());
  DistDecrypt(mf, cmf, sk, P);

  // Step 6
  if (P.whoami() == 0)
    {
      sub(m, mf, f);
    }
  else
    {
      m= f;
      m.negate();
    }

  // Step 7
  unsigned char sd[SEED_SIZE]= {0};
  PRNG G;
  G.SetSeed(sd);
  Random_Coins rc(cm.get_params());
  rc.generate(G);
  pk.encrypt(cc, mf, rc);
  // And again
  if (cf.level() == 0)
    {
      cc.Scale(m.get_field().get_prime());
    }
  sub(cc, cc, cf);
}

/* Reshare without generating a new ciphertext */
void Reshare(Plaintext &m, const Ciphertext &cm, const Player &P,
             const FHE_SK &sk)
{
  const FHE_Params &params= cm.get_params();

  vector<bigint> vv(params.phi_m()), ff(params.phi_m());
  sk.dist_decrypt_1a(vv, ff, cm, P.whoami());

  // Now pack into a string  for broadcasting
  if (P.whoami() != 0)
    {
      string os;
      for (unsigned int i= 0; i < params.phi_m(); i++)
        {
          outputBigint(os, vv[i]);
        }

      // Broadcast and Receive the values
      P.send_to_player(0, os);
    }
  else
    { // Reconstruct the value mod p0 from all shares
      vector<bigint> vv1(params.phi_m());
      for (unsigned int i= 1; i < P.nplayers(); i++)
        {
          string ss;
          P.receive_from_player(i, ss);
          for (unsigned int j= 0; j < params.phi_m(); j++)
            {
              inputBigint(ss, vv1[j]);
            }
          sk.dist_decrypt_2(vv, vv1);
        }
      for (unsigned int i= 0; i < params.phi_m(); i++)
        {
          ff[i]= ff[i] + vv[i];
        }
    }

  // Now get the final message
  m.set_poly_mod(ff, params.p0());
}

/* Extracts share data from a vector */
void get_share(vector<gfp> &s, vector<gfp> &macs, const Plaintext &aa,
               const vector<Plaintext> &cc, int i)
{
  s[0].assign(aa.element(i));
  for (unsigned int j= 0; j < macs.size(); j++)
    {
      macs[j].assign(cc[j].element(i));
    }
}

void offline_FHE_triples(Player &P, list<Share> &a, list<Share> &b,
                         list<Share> &c, const FHE_PK &pk, const FHE_SK &sk,
                         const FFT_Data &PTD,
                         FHE_Industry &industry)
{
  unsigned int nmacs= P.get_mac_keys().size();

  Plaintext va(PTD), vb(PTD), vc(PTD);
  vector<Plaintext> ga(nmacs, PTD), gb(nmacs, PTD), gc(nmacs, PTD);
  Ciphertext ca(pk.get_params()), cb(pk.get_params()), cc(pk.get_params()),
      nc(pk.get_params());
  Ciphertext tmp(pk.get_params());

  while (a.size() < sz_offline_batch)
    {
      industry.Next_Off_Production_Line(va, ca, P, "Triple a");
      industry.Next_Off_Production_Line(vb, cb, P, "Triple b");

      mul(cc, ca, cb, pk);

      Reshare(vc, nc, cc, P, pk, sk, industry);

      for (unsigned int i= 0; i < nmacs; i++)
        {
          mul(tmp, ca, industry.ct_mac(i), pk);
          Reshare(ga[i], tmp, P, sk);
          mul(tmp, cb, industry.ct_mac(i), pk);
          Reshare(gb[i], tmp, P, sk);
          mul(tmp, nc, industry.ct_mac(i), pk);
          Reshare(gc[i], tmp, P, sk);
        }

      vector<gfp> s(1), macs(nmacs);
      Share ss;
      for (unsigned int i= 0; i < pk.get_params().phi_m(); i++)
        {
          get_share(s, macs, va, ga, i);
          ss.assign(P.whoami(), s, macs);
          a.push_back(ss);
          get_share(s, macs, vb, gb, i);
          ss.assign(P.whoami(), s, macs);
          b.push_back(ss);
          get_share(s, macs, vc, gc, i);
          ss.assign(P.whoami(), s, macs);
          c.push_back(ss);
        }
    }
}



// Linear protocol, since doing manual keygen for all parties currently doesn't seem to be supported.
void offline_FHE_Semihonest_triples(Player &P, list<Share> &a, list<Share> &b,
                         list<Share> &c, const FHE_PK &pk, const FHE_SK &sk,
                         const FFT_Data &PTD,
                         FHE_Industry &industry) {

  PRNG G;
  unsigned char seed[SEED_SIZE];
  memset(seed, 0, SEED_SIZE);
  G.SetSeed(seed);
  const FHE_Params &params = pk.get_params();
  int batch_size = 40;
  vector<Plaintext> pa(batch_size, PTD);
  vector<Plaintext> pb(batch_size, PTD);
  vector<Plaintext> pc(batch_size, PTD);
  vector<Plaintext> pf(batch_size, PTD);
  printf("Batch size: %d\n", batch_size);

  vector<Ciphertext> Ca(batch_size, pk.get_params());
  vector<Ciphertext> Cb(batch_size, pk.get_params());
  vector<Ciphertext> Cc(batch_size, pk.get_params());
  vector<Ciphertext> Cf(batch_size, pk.get_params());
  printf("Resized ciphertexts\n");
  
  int num_players = P.nplayers();
  int my_num = P.whoami();
  /*
  * Step 1: Generate a_i, b_i, f_i randomly
  */
  printf("sampling randomized a/b/f.\n");
  for (int i = 0; i < batch_size; i++) {
    pa[i].allocate_slots(PTD.get_prime());
    pb[i].allocate_slots(PTD.get_prime());
    pc[i].allocate_slots((bigint)PTD.get_prime() << 64);
    pf[i].allocate_slots(PTD.get_prime());
    pa[i].randomize(G);
    pb[i].randomize(G);
    pf[i].randomize(G);
  }
  
    
  printf("sampling randomized a/b/f done.\n");

  /*
  * Step 2: Encrypt a_i/b_i/f_i and prepare to send it out to the first party
  */
  printf("encrypting a/b/f.\n");

  PRNG G_array[omp_get_max_threads()];
  for(int i = 0; i < omp_get_max_threads(); i++){
    G_array[i].SetSeed(seed);
  }

  #pragma omp parallel for
  for(int i = 0; i < batch_size; i++){
    Random_Coins rc2(pk.get_params());
    int num = omp_get_thread_num();
    rc2.generate(G_array[num]);
    pk.encrypt(Ca[i], pa[i], rc2);
    rc2.generate(G_array[num]);
    pk.encrypt(Cb[i], pb[i], rc2);
    rc2.generate(G_array[num]);
    pk.encrypt(Cf[i], pf[i], rc2);
  }
  printf("encrypting a/b/f done.\n");

  
  /*
  * Step 3:
  * For the first party, receiving others' encrypted a_i/b_i/f_i, adding them together, multiplying a_i and b_i, adding f_i, and sending the resulting ciphertexts back (separate rounds)
  * For the rest of the parties, sending encrypted a_i/b_i/f_i and receiving the masked products back (separate rounds)
  */
  ThreadPool pool(64);
  if(my_num == 0) {
    vector<Ciphertext> Ca_others[num_players];
    vector<Ciphertext> Cb_others[num_players];
    vector<Ciphertext> Cf_others[num_players];

    for(int i = 0; i < num_players; i++) {
      Ca_others[i].resize(batch_size, pk.get_params());
      Cb_others[i].resize(batch_size, pk.get_params());
      Cf_others[i].resize(batch_size, pk.get_params());
    }

    Ca_others[0] = Ca;
    Cb_others[0] = Cb;
    Cf_others[0] = Cf;
    vector<future<void>> res;

    printf("Size of these ciphertext vectors: %d %d %d\n", Ca_others[1].size(), Cb_others[1].size(), Cf_others[1].size());
    for(int j = 1; j < num_players; j++){
      int party = j;
      res.push_back(pool.enqueue([party, &P, &Ca_others, &Cb_others, &Cf_others, batch_size, &pk]() {
        string recv_ca;
        string recv_cb;
        string recv_cf;
        P.receive_from_player(party, recv_ca);
        P.receive_from_player(party, recv_cb);
        P.receive_from_player(party, recv_cf);
        istringstream ca_stream;
        istringstream cb_stream;
        istringstream cf_stream;
        ca_stream.str(recv_ca);
        cb_stream.str(recv_cb);
        cf_stream.str(recv_cf);

        printf("Receiving ca size: %d\n", recv_ca.size());
        for (int i = 0; i < batch_size; i++) {
          Ciphertext a_i(pk.get_params());
          Ciphertext b_i(pk.get_params());
          Ciphertext f_i(pk.get_params());
          a_i.input(ca_stream);
          b_i.input(cb_stream);
          f_i.input(cf_stream);
          Ca_others[party].at(i) = a_i;
          Cb_others[party].at(i) = b_i;
          Cf_others[party].at(i) = f_i;
        }
        printf("Done receiving party %d a/b/f\n", party);
      }));
    }
    joinNclean(res);


    printf("Ciphertext check to make sure this stream shit is working.\n");
    printf("Party 1 Ca size: %d\n", Ca_others[1].size());
    //Ca_others[1][0].output(cout, true);
    stringstream temp;
    Ca_others[1][0].output(temp);
    printf("Size:of Party 1's first ciphertext %d \n", temp.str().size());
    printf("Party 1's first ciphertext: %s\n", temp.str());
    printf("party0: adding encrypted a/b/f.\n");
    //#pragma omp parallel for
    for(int i = 0; i < batch_size; i++){
      for(int j = 1; j < num_players; j++){
        add(Ca_others[0][i], Ca_others[0][i], Ca_others[j][i]);
        add(Cb_others[0][i], Cb_others[0][i], Cb_others[j][i]);
        add(Cf_others[0][i], Cf_others[0][i], Cf_others[j][i]);
      }
    }
    printf("party0: adding encrypted a/b/f done.\n");

    
    printf("party0: multiplying encrypted a/b.\n");
    #pragma omp parallel for
    for(int i = 0; i < batch_size; i++){
      mul(Cc[i], Ca_others[0][i], Cb_others[0][i], pk);
    }
    printf("party0: multiplying encrypted a/b done.\n");
    

    printf("party0: masking c with f.\n");
    #pragma omp parallel for
    for(int i = 0; i < batch_size; i++){
      if(Cc[i].level()==0){
        Cf_others[0][i].Scale(PTD.get_prime());
      }
      add(Cc[i], Cf_others[0][i], Cc[i]);
    }
    printf("party0: masking c with f done.\n");
    // Cc[i] = Cc[i] + sum Cf[i]

    printf("party0: sending out masked c+f.\n");
    vector<future<void>> res2;
    for(int j = 1; j < num_players; j++){
      int party = j;
      res2.push_back(pool.enqueue([party, &P, &Cc, batch_size]() {
        ostringstream Cf_stream;
        for (int i = 0; i < batch_size; i++) {
          Cc[i].output(Cf_stream);
        }
        string cf_str = Cf_stream.str();
        P.send_to_player(party, cf_str);
      }));
    }
    joinNclean(res2);
    printf("party0: sending out masked c+f done.\n");
  } else {
    //printf("Ciphertext check to make sure this stream shit is working.\n");
    //Ca[0].output(cout, true);
    string send_ca;
    string send_cb;
    string send_cf;
    stringstream ca_stream;
    stringstream cb_stream;
    stringstream cf_stream;
    printf("Here\n");
    for (int i = 0; i < batch_size; i++) {
      Ca[i].output(ca_stream);
      Cb[i].output(cb_stream);
      Cf[i].output(cf_stream);
    }
    printf("Here after outputting\n");
    send_ca = ca_stream.str();
    send_cb = cb_stream.str();
    send_cf = cf_stream.str();
    printf("Send_ca size: %d\n", send_ca.size());
    P.send_to_player(0, send_ca);
    P.send_to_player(0, send_cb);
    P.send_to_player(0, send_cf);
    printf("Sent a/b/f to party 0 done!\n");


    string recv_cf;
    P.receive_from_player(0, recv_cf);
    istringstream input_cf_stream;
    input_cf_stream.str(recv_cf);
    for (int i = 0; i < batch_size; i++) {
      Ciphertext cf_i(pk.get_params());
      cf_i.input(input_cf_stream);
      Cc.at(i) = cf_i;
    }
    printf("receiving masked c+f done.\n");
  }

  /*
  * Step 4:
  * For the first party, receiving others' partial decryption vv, decrypting those ciphertexts as c, and saving c = c - f.
  * For the rest of the parties, sending the partial decryption vv and saving c = -f
  */

  
  printf("Before DistDecrypt\n");
  for (int i = 0; i < batch_size; i++) {
      // Decrypt Cc[i] and put into pc[i].
      DistDecrypt(pc[i], Cc[i], sk, P);
  }
  

  printf("making distributed decryption.\n");

  if (my_num == 0) {
    for (int i = 0; i < batch_size; i++) {
      // Set c = c-f
      sub(pc[i], pc[i], pf[i]);
    }
    printf("party 0: setting c = c - f_0 done.\n");
  } else {
    for(int i = 0; i < batch_size; i++) {
      pf[i].negate();
      pc[i] = pf[i];
    }
    printf("setting c = - f_i done.\n");
  }

  // Make shares
  vector<gfp> macs(0);
  for (int i = 0; i < batch_size; i++)
  {
    for (int j = 0; j < (int) pk.get_params().phi_m(); j++) {
      Share ss_a(my_num);
      ss_a.assign(pa[i].element(j), macs);
      a.push_back(ss_a);
      Share ss_b(my_num);
      ss_b.assign(pb[i].element(j), macs);
      b.push_back(ss_b);
      Share ss_c(my_num);
      ss_c.assign(pc[i].element(j), macs);
      c.push_back(ss_c);
    }
  }
  // Check if triples work.
  // Send triple

  vector<future<void>> res;
  gfp share_a = a.front().get_share(0);
  gfp share_b = b.front().get_share(0);
  gfp share_c = c.front().get_share(0);
  if (my_num == 0) {
    cout << "Share a: " << share_a << " Share b:" << share_b << " Share c: " << share_c << endl;
    res.push_back(pool.enqueue([&P, &share_a, &share_b, &share_c]() {
        string recv_ca;
        string recv_cb;
        string recv_cc;
        P.receive_from_player(1 - P.whoami(), recv_ca);
        P.receive_from_player(1 - P.whoami(), recv_cb);
        P.receive_from_player(1 - P.whoami(), recv_cc);
        istringstream ca_stream;
        istringstream cb_stream;
        istringstream cc_stream;
        ca_stream.str(recv_ca);
        cb_stream.str(recv_cb);
        cc_stream.str(recv_cc);
        gfp sa;
        gfp sb;
        gfp sc;
        sa.input(ca_stream, false);
        sb.input(cb_stream, false);
        sc.input(cc_stream, false);
        cout << "Sum of a: " << share_a + sa << endl;
        cout << "Sum of b: " << share_b + sb << endl;
        cout << "Mul result: " << (share_a + sa) * (share_b + sb) << endl;
        cout << "Sum of c, should match mul result: " << sc + share_c << endl;
    }));
  } else {
    res.push_back(pool.enqueue([&P, &a, &b, &c]() {
      ostringstream ca_stream;
      ostringstream cb_stream;
      ostringstream cc_stream;
      a.front().get_share(0).output(ca_stream, false);
      b.front().get_share(0).output(cb_stream, false);
      c.front().get_share(0).output(cc_stream, false);
      P.send_to_player(1 - P.whoami(), ca_stream.str());
      P.send_to_player(1 - P.whoami(), cb_stream.str());
      P.send_to_player(1 - P.whoami(), cc_stream.str());
    }));
  }
  

  

  joinNclean(res);
  printf("synchronizing all parties to end.\n");
  vector<string> os(P.nplayers());
  os[P.whoami()] = "true";
  P.Broadcast_Receive(os);
  printf("synchronizing all parties to end done.\n");
  printf("Done with triples! \n");
}


void offline_FHE_squares(Player &P, list<Share> &a, list<Share> &b,
                         const FHE_PK &pk, const FHE_SK &sk,
                         const FFT_Data &PTD,
                         FHE_Industry &industry)
{
  unsigned int nmacs= P.get_mac_keys().size();

  Plaintext va(PTD), vc(PTD);
  vector<Plaintext> ga(nmacs, PTD), gc(nmacs, PTD);
  Ciphertext ca(pk.get_params()), cc(pk.get_params()), nc(pk.get_params());
  Ciphertext tmp(pk.get_params());

  while (a.size() < sz_offline_batch)
    {
      industry.Next_Off_Production_Line(va, ca, P, "Square a");

      mul(cc, ca, ca, pk);

      Reshare(vc, nc, cc, P, pk, sk, industry);

      for (unsigned int i= 0; i < nmacs; i++)
        {
          mul(tmp, ca, industry.ct_mac(i), pk);
          Reshare(ga[i], tmp, P, sk);
          mul(tmp, nc, industry.ct_mac(i), pk);
          Reshare(gc[i], tmp, P, sk);
        }

      vector<gfp> s(1), macs(nmacs);
      Share ss;
      for (unsigned int i= 0; i < pk.get_params().phi_m(); i++)
        {
          get_share(s, macs, va, ga, i);
          ss.assign(P.whoami(), s, macs);
          a.push_back(ss);
          get_share(s, macs, vc, gc, i);
          ss.assign(P.whoami(), s, macs);
          b.push_back(ss);
        }
    }
}

void offline_FHE_bits(Player &P, list<Share> &a, const FHE_PK &pk,
                      const FHE_SK &sk, const FFT_Data &PTD,
                      FHE_Industry &industry)
{
  unsigned int nmacs= P.get_mac_keys().size();

  Plaintext va(PTD), vb(PTD);
  vector<Plaintext> ga(nmacs, PTD);
  Ciphertext ca(pk.get_params()), cb(pk.get_params());
  Ciphertext tmp(pk.get_params());

  while (a.size() < sz_offline_batch)
    { // First run the square protocol (no
      // need to get MACs on the b=a^2 values)
      industry.Next_Off_Production_Line(va, ca, P, "Bit a");

      for (unsigned int i= 0; i < nmacs; i++)
        {
          mul(tmp, ca, industry.ct_mac(i), pk);
          Reshare(ga[i], tmp, P, sk);
        }

      mul(cb, ca, ca, pk);

      // Just decrypt cb=ca^2 now
      DistDecrypt(vb, cb, sk, P);

      // Now divide a by sqrt{b} when b<>0
      vector<gfp> s(1), macs(nmacs);
      Share ss;
      gfp a2, one= 1, twoi= 2;
      twoi.invert();
      for (unsigned int i= 0; i < pk.get_params().phi_m(); i++)
        {
          a2.assign(vb.element(i));
          if (!a2.is_zero())
            {
              get_share(s, macs, va, ga, i);
              ss.assign(P.whoami(), s, macs);
              a2= a2.sqrRoot();
              a2.invert();
              ss.mul(ss, a2);
              ss.add(ss, one, P.get_mac_keys());
              ss.mul(ss, twoi);
              a.push_back(ss);
            }
        }
    }
}

void offline_FHE_IO(Player &P, unsigned int player_num, list<Share> &a,
                    list<gfp> &opened, const FHE_PK &pk, const FHE_SK &sk,
                    const FFT_Data &PTD, Open_Protocol &OP,
                    FHE_Industry &industry)
{
  unsigned int nmacs= P.get_mac_keys().size();

  Plaintext va(PTD);
  vector<Plaintext> ga(nmacs, PTD);
  Ciphertext ca(pk.get_params());
  Ciphertext tmp(pk.get_params());

  // First run the square protocol (no need to get MACs on the b=a^2 values)
  industry.Next_Off_Production_Line(va, ca, P, "IO a");

  for (unsigned int i= 0; i < nmacs; i++)
    {
      mul(tmp, ca, industry.ct_mac(i), pk);
      Reshare(ga[i], tmp, P, sk);
    }
  unsigned int sz= pk.get_params().phi_m();
  vector<Share> alist(sz);
  vector<gfp> openedlist(sz);

  vector<gfp> s(1), macs(nmacs);
  Share ss;
  for (unsigned int i= 0; i < sz; i++)
    {
      get_share(s, macs, va, ga, i);
      ss.assign(P.whoami(), s, macs);
      a.push_back(ss);
      alist[i]= ss;
    }

  if (P.whoami() != player_num)
    {
      OP.Open_To_One_Begin(player_num, alist, P);
    }
  else
    {
      OP.Open_To_One_End(openedlist, alist, P);
      for (unsigned int i= 0; i < sz; i++)
        {
          opened.push_back(openedlist[i]);
        }
    }
}
