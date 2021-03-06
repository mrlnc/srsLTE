/**
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srsenb/hdr/stack/rrc/rrc_nr.h"
#include "srsenb/hdr/common/common_enb.h"
#include "srsran/asn1/rrc_nr_utils.h"
#include "srsran/common/common_nr.h"

using namespace asn1::rrc_nr;

namespace srsenb {

rrc_nr::rrc_nr(srsran::timer_handler* timers_) : logger(srslog::fetch_basic_logger("RRC-NR")), timers(timers_) {}

int rrc_nr::init(const rrc_nr_cfg_t&     cfg_,
                 phy_interface_stack_nr* phy_,
                 mac_interface_rrc_nr*   mac_,
                 rlc_interface_rrc_nr*   rlc_,
                 pdcp_interface_rrc_nr*  pdcp_,
                 ngap_interface_rrc_nr*  ngap_,
                 gtpu_interface_rrc_nr*  gtpu_)
{
  phy  = phy_;
  mac  = mac_;
  rlc  = rlc_;
  pdcp = pdcp_;
  gtpu = gtpu_;
  ngap = ngap_;

  // TODO: overwriting because we are not passing config right now
  cfg = update_default_cfg(cfg_);

  // config logging
  logger.set_level(srslog::str_to_basic_level(cfg.log_level));
  logger.set_hex_dump_max_size(cfg.log_hex_limit);

  // derived
  slot_dur_ms = 1;

  if (generate_sibs() != SRSRAN_SUCCESS) {
    logger.error("Couldn't generate SIB messages.");
    return SRSRAN_ERROR;
  }

  config_mac();

  // add dummy user
  logger.info("Creating dummy DRB for RNTI=%d on LCID=%d", cfg.coreless.rnti, cfg.coreless.drb_lcid);
  add_user(cfg.coreless.rnti);
  srsran::rlc_config_t rlc_cnfg = srsran::rlc_config_t::default_rlc_um_nr_config(6);
  rlc->add_bearer(cfg.coreless.rnti, cfg.coreless.drb_lcid, rlc_cnfg);
  srsran::pdcp_config_t pdcp_cnfg{cfg.coreless.drb_lcid,
                                  srsran::PDCP_RB_IS_DRB,
                                  srsran::SECURITY_DIRECTION_DOWNLINK,
                                  srsran::SECURITY_DIRECTION_UPLINK,
                                  srsran::PDCP_SN_LEN_18,
                                  srsran::pdcp_t_reordering_t::ms500,
                                  srsran::pdcp_discard_timer_t::infinity,
                                  false,
                                  srsran::srsran_rat_t::nr};
  pdcp->add_bearer(cfg.coreless.rnti, cfg.coreless.drb_lcid, pdcp_cnfg);

  logger.info("Started");

  running = true;

  return SRSRAN_SUCCESS;
}

void rrc_nr::stop()
{
  if (running) {
    running = false;
  }
  users.clear();
}

template <class T>
void rrc_nr::log_rrc_message(const std::string&           source,
                             const direction_t            dir,
                             const srsran::byte_buffer_t* pdu,
                             const T&                     msg)
{
  if (logger.debug.enabled()) {
    asn1::json_writer json_writer;
    msg.to_json(json_writer);
    logger.debug(pdu->msg,
                 pdu->N_bytes,
                 "%s - %s %s (%d B)",
                 source.c_str(),
                 dir == Tx ? "Tx" : "Rx",
                 msg.msg.c1().type().to_string(),
                 pdu->N_bytes);
    logger.debug("Content:\n%s", json_writer.to_string().c_str());
  } else if (logger.info.enabled()) {
    logger.info(
        "%s - %s %s (%d B)", source.c_str(), dir == Tx ? "Tx" : "Rx", msg.msg.c1().type().to_string(), pdu->N_bytes);
  }
}

rrc_nr_cfg_t rrc_nr::update_default_cfg(const rrc_nr_cfg_t& current)
{
  // NOTE: This function is temporary.
  rrc_nr_cfg_t cfg_default = current;

  // Fill MIB
  cfg_default.mib.sub_carrier_spacing_common.value = mib_s::sub_carrier_spacing_common_opts::scs15or60;
  cfg_default.mib.ssb_subcarrier_offset            = 0;
  cfg_default.mib.intra_freq_resel.value           = mib_s::intra_freq_resel_opts::allowed;
  cfg_default.mib.cell_barred.value                = mib_s::cell_barred_opts::not_barred;
  cfg_default.mib.pdcch_cfg_sib1.search_space_zero = 0;
  cfg_default.mib.pdcch_cfg_sib1.ctrl_res_set_zero = 0;
  cfg_default.mib.dmrs_type_a_position.value       = mib_s::dmrs_type_a_position_opts::pos2;
  cfg_default.mib.sys_frame_num.from_number(0);

  cfg_default.cell.nof_prb         = 25;
  cfg_default.cell.nof_ports       = 1;
  cfg_default.cell.id              = 0;
  cfg_default.cell.cp              = SRSRAN_CP_NORM;
  cfg_default.cell.frame_type      = SRSRAN_FDD;
  cfg_default.cell.phich_length    = SRSRAN_PHICH_NORM;
  cfg_default.cell.phich_resources = SRSRAN_PHICH_R_1;

  // Fill SIB1
  cfg_default.sib1.cell_access_related_info.plmn_id_list.resize(1);
  cfg_default.sib1.cell_access_related_info.plmn_id_list[0].plmn_id_list.resize(1);
  srsran::plmn_id_t plmn;
  plmn.from_string("90170");
  srsran::to_asn1(&cfg_default.sib1.cell_access_related_info.plmn_id_list[0].plmn_id_list[0], plmn);
  cfg_default.sib1.cell_access_related_info.plmn_id_list[0].cell_id.from_number(1);
  cfg_default.sib1.cell_access_related_info.plmn_id_list[0].cell_reserved_for_oper.value =
      plmn_id_info_s::cell_reserved_for_oper_opts::not_reserved;
  cfg_default.sib1.si_sched_info_present                                  = true;
  cfg_default.sib1.si_sched_info.si_request_cfg.rach_occasions_si_present = true;
  cfg_default.sib1.si_sched_info.si_request_cfg.rach_occasions_si.rach_cfg_si.ra_resp_win.value =
      rach_cfg_generic_s::ra_resp_win_opts::sl8;
  cfg_default.sib1.si_sched_info.si_win_len.value = si_sched_info_s::si_win_len_opts::s20;
  cfg_default.sib1.si_sched_info.sched_info_list.resize(1);
  cfg_default.sib1.si_sched_info.sched_info_list[0].si_broadcast_status.value =
      sched_info_s::si_broadcast_status_opts::broadcasting;
  cfg_default.sib1.si_sched_info.sched_info_list[0].si_periodicity.value = sched_info_s::si_periodicity_opts::rf16;
  cfg_default.sib1.si_sched_info.sched_info_list[0].sib_map_info.resize(1);
  // scheduling of SI messages
  cfg_default.sib1.si_sched_info.sched_info_list[0].sib_map_info[0].type.value = sib_type_info_s::type_opts::sib_type2;
  cfg_default.sib1.si_sched_info.sched_info_list[0].sib_map_info[0].value_tag_present = true;
  cfg_default.sib1.si_sched_info.sched_info_list[0].sib_map_info[0].value_tag         = 0;

  // Fill SIB2+
  cfg_default.nof_sibs                     = 1;
  sib2_s& sib2                             = cfg_default.sibs[0].set_sib2();
  sib2.cell_resel_info_common.q_hyst.value = sib2_s::cell_resel_info_common_s_::q_hyst_opts::db5;
  // TODO: Fill SIB2 values

  // set loglevel
  cfg_default.log_level     = "debug";
  cfg_default.log_hex_limit = 10000;

  return cfg_default;
}

// This function is called from PRACH worker (can wait)
void rrc_nr::add_user(uint16_t rnti)
{
  if (users.count(rnti) == 0) {
    users.insert(std::make_pair(rnti, std::unique_ptr<ue>(new ue(this, rnti))));
    rlc->add_user(rnti);
    pdcp->add_user(rnti);
    logger.info("Added new user rnti=0x%x", rnti);
  } else {
    logger.error("Adding user rnti=0x%x (already exists)", rnti);
  }
}

void rrc_nr::config_mac()
{
  // Fill MAC scheduler configuration for SIBs
  srsenb::sched_interface::cell_cfg_t sched_cfg;
  set_sched_cell_cfg_sib1(&sched_cfg, cfg.sib1);

  // set SIB length
  for (uint32_t i = 0; i < nof_si_messages + 1; i++) {
    sched_cfg.sibs[i].len = sib_buffer[i]->N_bytes;
  }

  // PUCCH width
  sched_cfg.nrb_pucch = SRSRAN_MAX(cfg.sr_cfg.nof_prb, cfg.cqi_cfg.nof_prb);
  logger.info("Allocating %d PRBs for PUCCH", sched_cfg.nrb_pucch);

  // Copy Cell configuration
  sched_cfg.cell = cfg.cell;

  // Configure MAC scheduler
  mac->cell_cfg(&sched_cfg);
}

int32_t rrc_nr::generate_sibs()
{
  // MIB packing
  bcch_bch_msg_s mib_msg;
  mib_s&         mib = mib_msg.msg.set_mib();
  mib                = cfg.mib;
  {
    srsran::unique_byte_buffer_t mib_buf = srsran::make_byte_buffer();
    if (mib_buf == nullptr) {
      logger.error("Couldn't allocate PDU in %s().", __FUNCTION__);
      return SRSRAN_ERROR;
    }
    asn1::bit_ref bref(mib_buf->msg, mib_buf->get_tailroom());
    mib_msg.pack(bref);
    mib_buf->N_bytes = bref.distance_bytes();
    logger.debug(mib_buf->msg, mib_buf->N_bytes, "MIB payload (%d B)", mib_buf->N_bytes);
    mib_buffer = std::move(mib_buf);
  }

  si_sched_info_s::sched_info_list_l_& sched_info = cfg.sib1.si_sched_info.sched_info_list;
  uint32_t nof_messages = cfg.sib1.si_sched_info_present ? cfg.sib1.si_sched_info.sched_info_list.size() : 0;

  // msg is array of SI messages, each SI message msg[i] may contain multiple SIBs
  // all SIBs in a SI message msg[i] share the same periodicity
  sib_buffer.reserve(nof_messages + 1);
  asn1::dyn_array<bcch_dl_sch_msg_s> msg(nof_messages + 1);

  // Copy SIB1 to first SI message
  msg[0].msg.set_c1().set_sib_type1() = cfg.sib1;

  // Copy rest of SIBs
  for (uint32_t sched_info_elem = 0; sched_info_elem < nof_messages; sched_info_elem++) {
    uint32_t msg_index = sched_info_elem + 1; // first msg is SIB1, therefore start with second

    msg[msg_index].msg.set_c1().set_sys_info().crit_exts.set_sys_info();
    auto& sib_list = msg[msg_index].msg.c1().sys_info().crit_exts.sys_info().sib_type_and_info;

    for (uint32_t mapping = 0; mapping < sched_info[sched_info_elem].sib_map_info.size(); ++mapping) {
      uint32_t sibidx = sched_info[sched_info_elem].sib_map_info[mapping].type; // SIB2 == 0
      sib_list.push_back(cfg.sibs[sibidx]);
    }
  }

  // Pack payload for all messages
  for (uint32_t msg_index = 0; msg_index < nof_messages + 1; msg_index++) {
    srsran::unique_byte_buffer_t sib = srsran::make_byte_buffer();
    if (sib == nullptr) {
      logger.error("Couldn't allocate PDU in %s().", __FUNCTION__);
      return SRSRAN_ERROR;
    }
    asn1::bit_ref bref(sib->msg, sib->get_tailroom());
    msg[msg_index].pack(bref);
    sib->N_bytes = bref.distance_bytes();
    sib_buffer.push_back(std::move(sib));

    // Log SIBs in JSON format
    log_rrc_message("SIB payload", Tx, sib_buffer.back().get(), msg[msg_index]);
  }

  nof_si_messages = sib_buffer.size() - 1;

  return SRSRAN_SUCCESS;
}

/*******************************************************************************
  MAC interface
*******************************************************************************/

int rrc_nr::read_pdu_bcch_bch(const uint32_t tti, srsran::unique_byte_buffer_t& buffer)
{
  if (mib_buffer == nullptr || buffer->get_tailroom() < mib_buffer->N_bytes) {
    return SRSRAN_ERROR;
  }
  memcpy(buffer->msg, mib_buffer->msg, mib_buffer->N_bytes);
  buffer->N_bytes = mib_buffer->N_bytes;
  return SRSRAN_SUCCESS;
}

int rrc_nr::read_pdu_bcch_dlsch(uint32_t sib_index, srsran::unique_byte_buffer_t& buffer)
{
  if (sib_index >= sib_buffer.size()) {
    logger.error("SIB %d is not a configured SIB.", sib_index);
    return SRSRAN_ERROR;
  }

  if (buffer->get_tailroom() < sib_buffer[sib_index]->N_bytes) {
    logger.error("Not enough space to fit SIB %d into buffer (%d < %d)",
                 sib_index,
                 buffer->get_tailroom(),
                 sib_buffer[sib_index]->N_bytes);
    return SRSRAN_ERROR;
  }

  memcpy(buffer->msg, sib_buffer[sib_index]->msg, sib_buffer[sib_index]->N_bytes);
  buffer->N_bytes = sib_buffer[sib_index]->N_bytes;

  return SRSRAN_SUCCESS;
}

void rrc_nr::get_metrics(srsenb::rrc_metrics_t& m)
{
  // return metrics
}

void rrc_nr::handle_pdu(uint16_t rnti, uint32_t lcid, srsran::unique_byte_buffer_t pdu)
{
  if (pdu) {
    logger.info(pdu->msg, pdu->N_bytes, "Rx %s PDU", get_rb_name(lcid));
  }

  if (users.count(rnti) == 1) {
    switch (static_cast<srsran::nr_srb>(lcid)) {
      case srsran::nr_srb::srb0:
        //        parse_ul_ccch(rnti, std::move(pdu));
        break;
      case srsran::nr_srb::srb1:
      case srsran::nr_srb::srb2:
        //        parse_ul_dcch(p.rnti, p.lcid, std::move(p.pdu));
        break;
      default:
        logger.error("Rx PDU with invalid bearer id: %d", lcid);
        break;
    }
  } else {
    logger.warning("Discarding PDU for removed rnti=0x%x", rnti);
  }
}

/*******************************************************************************
  PDCP interface
*******************************************************************************/
void rrc_nr::write_pdu(uint16_t rnti, uint32_t lcid, srsran::unique_byte_buffer_t pdu)
{
  handle_pdu(rnti, lcid, std::move(pdu));
}

/*******************************************************************************
  UE class

  Every function in UE class is called from a mutex environment thus does not
  need extra protection.
*******************************************************************************/
rrc_nr::ue::ue(rrc_nr* parent_, uint16_t rnti_) : parent(parent_), rnti(rnti_)
{
  // setup periodic RRCSetup send
  rrc_setup_periodic_timer = parent->timers->get_unique_timer();
  rrc_setup_periodic_timer.set(5000, [this](uint32_t tid) {
    send_connection_setup();
    rrc_setup_periodic_timer.run();
  });
  rrc_setup_periodic_timer.run();
}

void rrc_nr::ue::send_connection_setup()
{
  dl_ccch_msg_s dl_ccch_msg;
  dl_ccch_msg.msg.set_c1().set_rrc_setup().rrc_transaction_id = ((transaction_id++) % 4u);
  rrc_setup_ies_s&    setup  = dl_ccch_msg.msg.c1().rrc_setup().crit_exts.set_rrc_setup();
  radio_bearer_cfg_s& rr_cfg = setup.radio_bearer_cfg;

  // Add DRB1 to cfg
  rr_cfg.drb_to_add_mod_list_present = true;
  rr_cfg.drb_to_add_mod_list.resize(1);
  auto& drb_item                               = rr_cfg.drb_to_add_mod_list[0];
  drb_item.drb_id                              = 1;
  drb_item.pdcp_cfg_present                    = true;
  drb_item.pdcp_cfg.ciphering_disabled_present = true;
  //  drb_item.cn_assoc_present = true;
  //  drb_item.cn_assoc.set_eps_bearer_id() = ;
  drb_item.recover_pdcp_present = false;

  // TODO: send config to RLC/PDCP

  send_dl_ccch(&dl_ccch_msg);
}

void rrc_nr::ue::send_dl_ccch(dl_ccch_msg_s* dl_ccch_msg)
{
  // Allocate a new PDU buffer, pack the message and send to PDCP
  srsran::unique_byte_buffer_t pdu = srsran::make_byte_buffer();
  if (pdu == nullptr) {
    parent->logger.error("Allocating pdu");
  }
  asn1::bit_ref bref(pdu->msg, pdu->get_tailroom());
  if (dl_ccch_msg->pack(bref) == asn1::SRSASN_ERROR_ENCODE_FAIL) {
    parent->logger.error("Failed to pack DL-CCCH message. Discarding msg.");
  }
  pdu->N_bytes = bref.distance_bytes();

  char buf[32] = {};
  sprintf(buf, "SRB0 - rnti=0x%x", rnti);
  parent->log_rrc_message(buf, Tx, pdu.get(), *dl_ccch_msg);
  parent->rlc->write_sdu(rnti, (uint32_t)srsran::nr_srb::srb0, std::move(pdu));
}

} // namespace srsenb
